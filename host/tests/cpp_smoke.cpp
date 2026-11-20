#include <array>
#include <cassert>
#include <acceldoku/stream_device.hpp>
#include <stdexcept>
#include <vector>

template <typename Exception, typename Function> void expect_error(Function fn) {
    bool thrown = false;
    try {
        fn();
    } catch (const Exception &) {
        thrown = true;
    }
    assert(thrown);
}

int main() {
    acceldoku::StreamDevice device("/dev/acceldoku", 1, 1);
    constexpr char digits[] =
        "534678912672195348198342567859761423426853791713924856961537284287419635345286179";
    std::array<acceldoku::PuzzleRecord, 3> puzzles{};
    for (unsigned n = 0; n < puzzles.size(); ++n) {
        puzzles[n][0] = n + 1;
        for (unsigned i = 0; i < 81; ++i) {
            puzzles[n][9 + i / 2] |= (digits[i] - '0') << ((i % 2) * 4);
        }
    }
    acceldoku::Puzzle puzzle{};
    for (unsigned i = 0; i < puzzle.size(); ++i) {
        puzzle[i] = digits[i] - '0';
    }
    auto encoded = acceldoku::encode(0x0123456789abcdefULL, puzzle);
    assert(acceldoku::record_id(encoded) == 0x0123456789abcdefULL);
    assert(acceldoku::record_status(encoded) == 0);
    assert(acceldoku::decode_cells(encoded) == puzzle);
    assert(encoded[49] < 16);

    expect_error<std::out_of_range>([&] { device.write_tx(1, puzzles); });
    expect_error<std::logic_error>([&] { device.enqueue_tx(0); });
    expect_error<std::logic_error>([&] { device.rx_size(0); });
    expect_error<std::logic_error>([&] { device.read_rx(0); });
    expect_error<std::out_of_range>([&] { device.read_rx(1); });
    expect_error<std::out_of_range>([&] { device.rx_size(1); });
    expect_error<std::invalid_argument>([&] { device.wait(static_cast<acceldoku::WaitFor>(0)); });
    expect_error<std::invalid_argument>([&] { device.wait(static_cast<acceldoku::WaitFor>(4)); });
    auto empty = device.try_dequeue_tx();
    assert(empty);
    device.write_tx(*empty, std::span(puzzles).first(1));
    device.write_tx(*empty, {});
    device.enqueue_tx(*empty);
    expect_error<std::logic_error>([&] { device.enqueue_tx(*empty); });
    empty = device.try_dequeue_tx();
    assert(empty);
    device.enqueue_tx(*empty);
    empty = device.try_dequeue_tx();
    assert(empty);
    device.write_tx(*empty, {});
    device.enqueue_tx(*empty);

    for (unsigned round = 0; round < 2; ++round) {
        if (round) {
            device.stream_on();
        }
        auto tx = device.try_dequeue_tx();
        assert(tx);
        device.write_tx(*tx, std::span(puzzles).first(1));
        device.write_tx(*tx, puzzles, 1);
        expect_error<std::overflow_error>([&] { device.write_tx(*tx, puzzles, UINT64_MAX); });
        std::vector<acceldoku::PuzzleRecord> oversized(acceldoku::StreamDevice::batch_capacity() + 1);
        expect_error<std::length_error>([&] { device.write_tx(*tx, oversized); });
        device.enqueue_tx(*tx);
        expect_error<std::logic_error>([&] { device.enqueue_tx(*tx); });
        expect_error<std::logic_error>([&] { device.write_tx(*tx, puzzles); });
        if (!round) {
            device.stream_on();
        }
        std::array<bool, 3> seen{};
        unsigned received = 0;
        while (received < 3) {
            device.wait(acceldoku::WaitFor::rx);
            if (auto rx = device.try_dequeue_rx()) {
                auto count = device.rx_size(*rx);
                auto result = device.read_rx(*rx);
                assert(result.size() == count);
                for (unsigned i = 0; i < count; ++i) {
                    auto id = result[i][0];
                    assert(id >= 1 && id <= 3 && !seen[id - 1]);
                    assert(result[i] == puzzles[id - 1]);
                    seen[id - 1] = true;
                }
                received += count;
                device.enqueue_rx(*rx);
                expect_error<std::logic_error>([&] { device.read_rx(*rx); });
                expect_error<std::logic_error>([&] { device.enqueue_rx(*rx); });
            }
        }
        device.stream_off();
    }

    auto initial = device.try_dequeue_tx();
    assert(initial);
    device.stream_on();
    device.write_tx(*initial, std::span(puzzles).first(1));
    device.enqueue_tx(*initial);
    device.wait(acceldoku::WaitFor::rx);
    auto held_rx = device.try_dequeue_rx();
    assert(held_rx && device.rx_size(*held_rx) == 1);
    auto held_view = device.read_rx(*held_rx);
    device.wait(acceldoku::WaitFor::tx);
    auto held_tx = device.try_dequeue_tx();
    assert(held_tx);
    device.stream_off();
    assert(!device.try_dequeue_tx());
    device.stream_on();
    assert(held_view[0] == puzzles[0]);
    device.enqueue_rx(*held_rx);
    device.write_tx(*held_tx, std::span(puzzles).first(1));
    device.enqueue_tx(*held_tx);
    device.wait(acceldoku::WaitFor::rx);
    auto next_rx = device.try_dequeue_rx();
    assert(next_rx && device.rx_size(*next_rx) == 1);
    assert(device.read_rx(*next_rx)[0] == puzzles[0]);
    device.enqueue_rx(*next_rx);
    device.stream_off();
}
