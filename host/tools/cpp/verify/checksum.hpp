#pragma once

#include <cstdint>
#include <acceldoku/stream_device.hpp>
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

// Four independent 32-bit word lanes followed by a 64-bit avalanche. The
// scalar and NEON paths interpret the wire bytes as little-endian words.
struct Checksum {
    std::uint64_t count = 0, sum = 0, xors = 0;

    void add(const acceldoku::PuzzleRecord &record) noexcept {
        std::uint32_t lanes[4] = {2166136261U, 2246822519U, 3266489917U, 668265263U};
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        auto state = vld1q_u32(lanes);
        const auto prime = vdupq_n_u32(16777619U);
        for (unsigned i = 0; i < 64; i += 16) {
            auto words = vreinterpretq_u32_u8(vld1q_u8(record.data() + i));
            state = vmulq_u32(veorq_u32(state, words), prime);
        }
        vst1q_u32(lanes, state);
#else
        for (unsigned i = 0; i < 64; i += 16) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                const auto p = i + lane * 4;
                std::uint32_t word = 0;
                for (unsigned j = 0; j < 4; ++j) {
                    word |= std::uint32_t(record[p + j]) << (8 * j);
                }
                lanes[lane] = (lanes[lane] ^ word) * 16777619U;
            }
        }
#endif
        std::uint64_t h = (std::uint64_t(lanes[0]) << 32) | lanes[1];
        h ^= ((std::uint64_t(lanes[2]) << 32) | lanes[3]) * 0x9e3779b97f4a7c15ULL;
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        ++count;
        sum += h;
        xors ^= h;
    }

    void merge(const Checksum &other) noexcept {
        count += other.count;
        sum += other.sum;
        xors ^= other.xors;
    }

    bool operator==(const Checksum &) const = default;
};
