#!/usr/bin/env python3
"""Exercise DMA ring wrap and buffer reuse through the real driver."""
import select
import struct

from acceldoku_device import Device, RECORDS


def main():
    # More buffers than the 32-entry DMA rings, including a partial final TX.
    buffers = 65
    lengths = [RECORDS] * (buffers - 1) + [19]
    total = sum(lengths)
    digits = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
    cells = sum(int(digit) << (4 * i) for i, digit in enumerate(digits)).to_bytes(41, "little")
    device = Device("/dev/acceldoku", buffers, buffers)
    try:
        next_id = 0
        for index, length in enumerate(lengths):
            data = bytearray(length * 64)
            for i in range(length):
                struct.pack_into("<Q", data, i * 64, next_id + i)
                data[i * 64 + 9:i * 64 + 50] = cells
            device.maps[0][index][:len(data)] = data
            device.queue(0, index, length)
            device.queue(1, index)
            next_id += length
        device.start()
        seen = bytearray(total)
        received = tx_done = 0
        while received < total or tx_done < buffers:
            events = (select.POLLIN if received < total else 0) | (select.POLLOUT if tx_done < buffers else 0)
            device.wait(events)
            while (done := device.dequeue(0)) is not None:
                tx_done += 1
            while (done := device.dequeue(1)) is not None:
                index, count = done
                for offset in range(0, count * 64, 64):
                    record = device.maps[1][index][offset:offset + 64]
                    record_id = struct.unpack_from("<Q", record)[0]
                    assert record_id < total and not seen[record_id]
                    assert record[8] == 0 and record[9:50] == cells and record[50:] == bytes(14)
                    seen[record_id] = 1
                received += count
        assert received == total and all(seen)
        assert all(value == total for value in device.stats()[:4])
        print(f"PASS: {buffers} buffers, {total} records across DMA rings")
    finally:
        device.close()


if __name__ == "__main__":
    main()
