#include "../stats.hpp"
#include <fades/stream_device.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static std::vector<fades::PuzzleRecord> load_puzzles(const char *path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open puzzle file");
    }
    std::vector<fades::PuzzleRecord> records;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) {
            continue;
        }
        auto end = line.find_last_not_of(" \t\r");
        line = line.substr(begin, end - begin + 1);
        if (line.size() != 81) {
            throw std::runtime_error("invalid puzzle length at line " +
                                     std::to_string(line_number));
        }
        fades::Puzzle puzzle{};
        for (std::size_t i = 0; i < puzzle.size(); ++i) {
            char c = line[i];
            if (c == '.') {
                puzzle[i] = 0;
            } else if (c >= '0' && c <= '9') {
                puzzle[i] = c - '0';
            } else {
                throw std::runtime_error("invalid puzzle character at line " +
                                         std::to_string(line_number));
            }
        }
        records.push_back(fades::encode(0, puzzle));
    }
    if (file.bad()) {
        throw std::runtime_error("cannot read puzzle file");
    }
    if (records.empty()) {
        throw std::runtime_error("empty puzzle file");
    }
    return records;
}

int main(int argc, char **argv) try {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " PUZZLES.txt TX_BUFFERS RX_BUFFERS SECONDS\n";
        return 1;
    }
    auto parse = [](const char *text, unsigned max) {
        std::size_t pos = 0;
        auto value = std::stoul(text, &pos);
        if (text[pos] || !value || value > max) {
            throw std::invalid_argument("invalid argument");
        }
        return static_cast<unsigned>(value);
    };
    unsigned tx = parse(argv[2], FADES_DMA_MAX_BUFFERS);
    unsigned rx = parse(argv[3], FADES_DMA_MAX_BUFFERS);
    unsigned seconds = parse(argv[4], 3600);
    auto input = load_puzzles(argv[1]);
    const std::size_t input_size = input.size();
    constexpr auto batch_capacity = fades::StreamDevice::batch_capacity();
    // Extend with the cyclic prefix so every full batch is one contiguous span.
    input.reserve(input_size + batch_capacity - 1);
    for (std::size_t i = 0; i < batch_capacity - 1; ++i) {
        input.push_back(input[i % input_size]);
    }

    fades::StreamDevice device("/dev/fades", tx, rx);
    std::uint64_t submitted = 0, received = 0;
    std::size_t position = 0;
    auto fill = [&](std::uint32_t index) {
        device.write_tx(index, std::span(input).subspan(position, batch_capacity), submitted);
        device.enqueue_tx(index);
        submitted += batch_capacity;
        position = (position + batch_capacity) % input_size;
    };
    // Initially CPU-owned buffers are not reported by poll.
    while (auto batch = device.try_dequeue_tx()) {
        fill(*batch);
    }
    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::seconds(seconds);
    device.stream_on();
    bool feeding = true;
    while (feeding || received < submitted) {
        device.wait(feeding ? fades::WaitFor::tx | fades::WaitFor::rx : fades::WaitFor::rx);
        if (Clock::now() >= deadline) {
            feeding = false;
        }
        if (feeding) {
            if (auto batch = device.try_dequeue_tx()) {
                fill(*batch);
            }
        }
        if (auto batch = device.try_dequeue_rx()) {
            received += device.rx_size(*batch);
            device.enqueue_rx(*batch);
        }
    }
    auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    auto stats = device.stats();
    if (stats.accepted != submitted || stats.emitted != submitted ||
        stats.dispatched != submitted || stats.collected != submitted) {
        throw std::runtime_error("solver statistics mismatch");
    }
    device.stream_off();
    std::cout << "TX buffers: " << tx << "\nRX buffers: " << rx << "\nRecords: " << received
              << "\nElapsed: " << elapsed << " s\nThroughput: " << received / elapsed / 1e6
              << " Mproblem/s\n";
    print_stats(std::cout, stats);
    return 0;
} catch (const std::exception &e) {

    std::cerr << e.what() << '\n';
    return 1;
}
