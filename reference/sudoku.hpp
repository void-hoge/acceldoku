#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// Board conversion and solution validation for the fastdoku reference.
namespace reference {
extern "C" unsigned fades_fastdoku_solve(const uint8_t* input, uint8_t* output, unsigned limit);
using Mask = uint16_t;
using Board = std::array<Mask, 81>;
constexpr Mask all = 0x1ff;
inline bool single(Mask m) { return m && !(m & (m - 1)); }
inline constexpr auto units = [] {
    std::array<std::array<unsigned, 9>, 27> result{};
    for (unsigned u = 0; u < 27; ++u)
        for (unsigned p = 0; p < 9; ++p)
            result[u][p] = u < 9 ? 9*u+p : u < 18 ? 9*p+u-9 :
                ((u-18)/3)*27 + ((u-18)%3)*3 + (p/3)*9 + p%3;
    return result;
}();

inline Board parse(const std::string& line) {
    std::string_view text(line);
    if (text.ends_with('\r')) text.remove_suffix(1); // getline on CRLF input
    if (text.size() != 81)
        throw std::runtime_error("Expected exactly 81 characters ('.' or '1'..'9')");
    Board board{};
    for (unsigned i = 0; i < board.size(); ++i) {
        const char digit = text[i];
        if (digit == '.') board[i] = all;
        else if (digit >= '1' && digit <= '9') board[i] = Mask(1u << (digit-'1'));
        else throw std::runtime_error("Invalid cell: expected '.' or '1'..'9'");
    }
    return board;
}

inline bool valid_solution(const Board& input, const Board& answer) {
    for (unsigned i = 0; i < 81; ++i)
        if (!single(answer[i]) || (answer[i] & input[i]) != answer[i]) return false;
    for (const auto& unit : units) {
        Mask used = 0;
        for (auto i : unit) {
            if (used & answer[i]) return false;
            used |= answer[i];
        }
        if (used != all) return false;
    }
    return true;
}

struct Result {
    unsigned solutions = 0;
    Board first{};
};
// Fastdoku accepts givens, rather than arbitrary intermediate candidate sets.
inline Result solve(const Board& board, unsigned limit = 1) {
    if (limit != 1 && limit != 2) throw std::invalid_argument("Solution limit must be 1 or 2");
    std::array<uint8_t, 81> input{}, output{};
    for (unsigned i = 0; i < 81; ++i) {
        if (board[i] == all) continue;
        if (!single(board[i]) || (board[i] & ~all))
            throw std::invalid_argument("Fastdoku requires givens or unrestricted cells");
        input[i] = static_cast<uint8_t>(std::countr_zero(board[i]) + 1);
    }
    Result result;
    result.solutions = fades_fastdoku_solve(input.data(), output.data(), limit);
    if (result.solutions) {
        for (unsigned i = 0; i < 81; ++i) {
            if (output[i] < 1 || output[i] > 9) throw std::runtime_error("Invalid fastdoku digit");
            result.first[i] = Mask(1u << (output[i] - 1));
        }
        if (!valid_solution(board, result.first)) throw std::runtime_error("Invalid fastdoku solution");
    }
    return result;
}
} // namespace reference
