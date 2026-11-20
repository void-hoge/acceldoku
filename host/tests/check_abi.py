#!/usr/bin/env python3
"""Check exclusive ownership and mmap lifetime on the real driver."""
import errno
import fcntl
import mmap
import os

from acceldoku_device import CONFIG, CONFIG_CMD, STRIDE, expect_errno, info, query


def main():
    path = "/dev/acceldoku"
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    mapping = None
    try:
        assert info(fd)[3:5] == (0, 0)
        expect_errno(errno.EBUSY, lambda: os.open(path, os.O_RDWR))
        fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(1, 1, 0, 0))
        offset, size = query(fd, 0, 0)
        assert size == STRIDE
        mapping = mmap.mmap(fd, size, flags=mmap.MAP_SHARED, offset=offset)
        mapping[:8] = b"test dma"
        os.close(fd)
        fd = None
        expect_errno(errno.EBUSY, lambda: os.open(path, os.O_RDWR))
        assert mapping[:8] == b"test dma"
    finally:
        if mapping is not None:
            mapping.close()
        if fd is not None:
            os.close(fd)
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    try:
        assert info(fd)[3:5] == (0, 0)
    finally:
        os.close(fd)
    print("PASS: exclusive open and mapping lifetime")


if __name__ == "__main__":
    main()
