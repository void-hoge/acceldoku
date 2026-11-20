#include "stats.hpp"
#include <acceldoku/stream_device.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<acceldoku::PuzzleRecord> load_puzzles(const char *path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open puzzle file");
    }
    constexpr auto max_records =
        std::size_t{ACCELDOKU_DMA_MAX_BUFFERS} * acceldoku::StreamDevice::batch_capacity();
    std::vector<acceldoku::PuzzleRecord> records;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.starts_with("\xef\xbb\xbf")) {
            line.erase(0, 3);
        }
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        acceldoku::Puzzle puzzle{};
        std::size_t cells = 0;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == '\r') {
                continue;
            }
            if (cells == puzzle.size() || (c != '.' && (c < '0' || c > '9'))) {
                throw std::runtime_error("invalid puzzle at line " + std::to_string(line_number));
            }
            puzzle[cells++] = c == '.' ? 0 : c - '0';
        }
        if (cells != puzzle.size()) {
            throw std::runtime_error("invalid puzzle at line " + std::to_string(line_number));
        }
        if (records.size() == max_records) {
            throw std::runtime_error("puzzle count exceeds one-shot buffer limit");
        }
        records.push_back(acceldoku::encode(records.size(), puzzle));
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
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " PUZZLES.txt\n";
        return 1;
    }
    const auto inputs = load_puzzles(argv[1]);
    const auto total = inputs.size();
    constexpr auto capacity = acceldoku::StreamDevice::batch_capacity();
    const auto buffers = static_cast<std::uint32_t>((total + capacity - 1) / capacity);
    acceldoku::StreamDevice device("/dev/acceldoku", buffers, buffers);

    for (std::size_t offset = 0; offset < total; offset += capacity) {
        const auto batch = device.try_dequeue_tx();
        if (!batch) {
            throw std::runtime_error("not enough TX buffers for the puzzle set");
        }
        const auto count = std::min(capacity, total - offset);
        device.write_tx(*batch, std::span(inputs).subspan(offset, count));
        device.enqueue_tx(*batch);
    }

    std::size_t received = 0;
    const auto begin = std::chrono::steady_clock::now();
    device.stream_on();
    while (received < total) {
        device.wait(acceldoku::WaitFor::rx);
        while (auto batch = device.try_dequeue_rx()) {
            received += device.rx_size(*batch);
            if (received > total) {
                throw std::runtime_error("received more records than submitted");
            }
            if (received == total) {
                break;
            }
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto stats = device.stats();
    device.stream_off();
    if (stats.accepted != total || stats.dispatched != total || stats.collected != total ||
        stats.emitted != total) {
        throw std::runtime_error("solver statistics mismatch");
    }
    std::cout << "TX buffers: " << buffers << "\nRX buffers: " << buffers
              << "\nRecords: " << received << "\nElapsed: " << elapsed
              << " s\nThroughput: " << received / elapsed / 1e6 << " Mproblem/s\n";
    print_stats(std::cout, stats);
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
