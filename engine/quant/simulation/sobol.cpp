#include "../kernel.h"
#include <array>
#include <cmath>
#include <numbers>

namespace quant {

// Joe-Kuo direction numbers for dimensions 1..21 (s, a, m values).
// Source: S. Joe and F. Y. Kuo, "Constructing Sobol Sequences with Better
// Two-Dimensional Projections", SIAM J. Sci. Comput. 30, 2635 (2008).
// Dimension 1 is the Van der Corput sequence (trivially 1-bit).

static constexpr int SOBOL_BITS = 32;
static constexpr int SOBOL_MAX_DIM = 21;

// Joe-Kuo table: {s, a, m[1..s]} for dimensions 2..21
// (Dimension 1 has all direction numbers = 1 after normalization.)
struct JKRow { int s; unsigned a; std::array<unsigned, 13> m; };

static const JKRow JK_TABLE[] = {
    { 1, 0,  {1} },
    { 2, 1,  {1, 1} },
    { 3, 1,  {1, 3, 7} },
    { 3, 2,  {1, 1, 5} },
    { 4, 1,  {1, 3, 1, 1} },
    { 4, 4,  {1, 1, 9, 3} },
    { 5, 2,  {1, 3, 27, 15, 29} },
    { 5, 4,  {1, 3, 7, 7, 21} },
    { 5, 7,  {1, 1, 11, 5, 27} },
    { 5, 11, {1, 1, 25, 5, 13} },
    { 5, 13, {1, 1, 19, 11, 25} },
    { 5, 14, {1, 3, 13, 25, 29} },
    { 6, 1,  {1, 3, 3, 9, 7, 23} },
    { 6, 13, {1, 1, 7, 13, 25, 5} },
    { 6, 16, {1, 3, 5, 25, 7, 27} },
    { 6, 19, {1, 1, 1, 15, 11, 29} },
    { 6, 22, {1, 3, 15, 29, 1, 9} },
    { 6, 25, {1, 1, 9, 5, 21, 3} },
    { 7, 1,  {1, 3, 31, 11, 5, 3, 21} },
    { 7, 4,  {1, 1, 5, 1, 17, 19, 1} },
};

static_assert(std::size(JK_TABLE) == SOBOL_MAX_DIM - 1,
              "JK_TABLE must have exactly SOBOL_MAX_DIM-1 rows");

SobolEngine::SobolEngine(int dimension)
    : dim_(dimension), counter_(0)
{
    if (dimension < 1 || dimension > SOBOL_MAX_DIM)
        dimension = 1;
    dim_ = dimension;

    V_.assign(dim_, std::vector<uint32_t>(SOBOL_BITS, 0));

    // Dimension 0: Van der Corput — V[0][bit] = 1 << (31 - bit)
    for (int bit = 0; bit < SOBOL_BITS; ++bit)
        V_[0][bit] = 1u << (SOBOL_BITS - 1 - bit);

    for (int d = 1; d < dim_; ++d) {
        const auto& row = JK_TABLE[d - 1];
        const int s = row.s;
        // Seed with primitive direction numbers (must be odd and < 2^i)
        for (int i = 0; i < s && i < SOBOL_BITS; ++i)
            V_[d][i] = row.m[i] << (SOBOL_BITS - 1 - i);
        // Recurrence: V[i] = V[i-s] XOR (V[i-s] >> s) XOR sum of a-bits
        for (int i = s; i < SOBOL_BITS; ++i) {
            uint32_t v = V_[d][i - s] ^ (V_[d][i - s] >> s);
            for (int k = 1; k < s; ++k) {
                if ((row.a >> (s - 1 - k)) & 1u)
                    v ^= V_[d][i - k];
            }
            V_[d][i] = v;
        }
    }

    X_.assign(dim_, 0u);
}

void SobolEngine::next(std::span<double> out_unit) {
    // Gray-code increment: find rightmost zero bit of counter
    const int c = __builtin_ctz(~counter_) % SOBOL_BITS;
    ++counter_;
    for (int d = 0; d < dim_ && d < static_cast<int>(out_unit.size()); ++d) {
        X_[d] ^= V_[d][c];
        out_unit[d] = static_cast<double>(X_[d]) / static_cast<double>(1ull << SOBOL_BITS);
    }
}

}  // namespace quant
