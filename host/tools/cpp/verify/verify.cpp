#include "checksum_workers.hpp"
#include <sudoku.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void set_id(fades::PuzzleRecord &record, std::uint64_t id) {
    for (unsigned i = 0; i < 8; ++i) {
        record[i] = static_cast<std::uint8_t>(id >> (8 * i));
    }
}

int main(int argc, char **argv) try {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " PUZZLES.txt\n";
        return 1;
    }
    constexpr unsigned tx = 4, rx = 4, threads = 4;
    std::ifstream file(argv[1]);
    if (!file) {
        throw std::runtime_error("cannot open puzzle file");
    }
    std::vector<fades::PuzzleRecord> inputs;
    Checksum expected;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        std::replace(line.begin(), line.end(), '0', '.');
        auto board = reference::parse(line);
        auto result = reference::solve(board, 2);
        if (result.solutions > 1) {
            throw std::runtime_error("multiple solutions at line " + std::to_string(line_number));
        }
        fades::Puzzle input{}, output{};
        for (unsigned i = 0; i < 81; ++i) {
            if (board[i] != reference::all) {
                input[i] = std::countr_zero(board[i]) + 1;
            }
            if (result.solutions) {
                output[i] = std::countr_zero(result.first[i]) + 1;
            }
        }
        inputs.push_back(fades::encode(0, input));
        auto answer = fades::encode(0, output);
        answer[8] = result.solutions ? 0 : 1;
        set_id(answer, inputs.size() - 1);
        expected.add(answer);
    }
    if (file.bad()) {
        throw std::runtime_error("cannot read puzzle file");
    }
    if (inputs.empty()) {
        throw std::runtime_error("empty puzzle file");
    }
    const auto total = inputs.size();
    constexpr auto capacity = fades::StreamDevice::batch_capacity();

    fades::StreamDevice device("/dev/fades", tx, rx);
    ChecksumWorkers workers(threads);
    std::uint64_t submitted = 0;
    Checksum actual;
    auto fill = [&](std::uint32_t index) {
        auto count = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, total - submitted));
        device.write_tx(index, std::span(inputs).subspan(submitted, count), submitted);
        device.enqueue_tx(index);
        submitted += count;
    };
    while (submitted < total) {
        auto batch = device.try_dequeue_tx();
        if (!batch) {
            break;
        }
        fill(*batch);
    }
    const auto begin = std::chrono::steady_clock::now();
    device.stream_on();
    while (actual.count < total) {
        device.wait(submitted < total ? fades::WaitFor::tx | fades::WaitFor::rx
                                      : fades::WaitFor::rx);
        if (submitted < total) {
            if (auto batch = device.try_dequeue_tx()) {
                fill(*batch);
            }
        }
        if (auto batch = device.try_dequeue_rx()) {
            actual.merge(workers.calculate(device.read_rx(*batch)));
            device.enqueue_rx(*batch);
        }
    }
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    auto stats = device.stats();
    device.stream_off();
    auto print = [](const char *name, const Checksum &c) {
        std::cout << name << ": count=" << c.count << " sum=" << std::hex << std::setfill('0')
                  << std::setw(16) << c.sum << " xor=" << std::setw(16) << c.xors << std::dec
                  << '\n';
    };
    print("expected", expected);
    print("actual  ", actual);
    std::cout << "threads=" << threads << ", elapsed=" << elapsed << " s, "
              << actual.count / elapsed / 1e6 << " Mproblem/s\n";
    if (!(actual == expected) || stats.accepted != total || stats.emitted != total ||
        stats.dispatched != total || stats.collected != total) {
        throw std::runtime_error("verification failed: checksum or solver statistics mismatch");
    }
    std::cout << "PASS\n";
    return 0;
} catch (const std::exception &e) {

    std::cerr << e.what() << '\n';
    return 1;
}
