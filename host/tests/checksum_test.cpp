#include "../tools/cpp/verify/checksum_workers.hpp"
#include <stdexcept>

int main() {
    fades::PuzzleRecord a{}, b{}, c{};
    a[0] = 1;
    b[8] = 1;
    c[49] = 7;
    Checksum reference, reordered, modified, duplicated, missing;
    reference.add(a);
    reference.add(b);
    reference.add(c);
    reordered.add(c);
    reordered.add(a);
    reordered.add(b);
    modified.add(a);
    modified.add(b);
    c[49] ^= 1;
    modified.add(c);
    duplicated.add(a);
    duplicated.add(a);
    duplicated.add(b);
    missing.add(a);
    missing.add(b);
    if (!(reference == reordered) || reference == modified || reference == duplicated ||
        reference == missing) {
        throw std::runtime_error("checksum test failed");
    }
    std::vector<fades::PuzzleRecord> records(257);
    for (std::size_t i = 0; i < records.size(); ++i) {
        for (std::size_t j = 0; j < records[i].size(); ++j) {
            records[i][j] = static_cast<std::uint8_t>(i * 73 + j * 19);
        }
    }
    Checksum serial;
    for (const auto &record : records) {
        serial.add(record);
    }
    if (serial.count != 257 || serial.sum != 0x95ebd6342ef55e6aULL ||
        serial.xors != 0xad250ed346dd01aaULL) {
        throw std::runtime_error("checksum test vector mismatch");
    }
    for (unsigned count : {1, 2, 4, 8}) {
        ChecksumWorkers workers(count);
        if (!(workers.calculate(records) == serial) || !(workers.calculate({}) == Checksum{}) ||
            !(workers.calculate(records) == serial)) {
            throw std::runtime_error("parallel checksum test failed");
        }
    }
}
