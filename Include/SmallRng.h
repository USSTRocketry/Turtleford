/**
 * @file SmallRng.h
 * @brief Small pseudo random number generator for production code: timing jitter and link
 * epochs. Not for cryptography.
 *
 * A thin wrapper over the standard library's engine, seed sequence and distribution. One word of
 * state, no entropy source: the caller supplies the seed. Seed() allocates briefly (std::seed_seq
 * keeps its input in a std::vector), so seed at setup, not in a loop. (ra::RNG in Carrot-River is
 * the heavy one, for tests.) The engine and the seed sequence are fully specified by the standard,
 * but uniform_int_distribution is not, so the values drawn for a given seed differ between standard
 * libraries: nothing may depend on exact values, only on the range, and on different seeds giving
 * unrelated sequences.
 */

#pragma once

#include <cstdint>
#include <random>

namespace ra::turtleford
{
/**
 * @brief A seeded generator of uniform integers, on std::minstd_rand.
 *
 * Not thread safe: each owner keeps its own. Copies continue the same sequence independently.
 */
class SmallRng
{
public:
    /** @param Value The seed, as for Seed(). */
    explicit SmallRng(uint32_t Value) { Seed(Value); }

    /**
     * @brief Start a new sequence. Equal seeds give equal sequences.
     * @param Value The seed. Any value is accepted, 0 included.
     */
    void Seed(uint32_t Value)
    {
        // The seeds in use are small and close together: the two ends' Seed*2 and Seed*2+1, device
        // addresses 1 and 2, consecutive boot counters. The engine is linear, so seeded directly,
        // two such engines stay a fixed distance apart at every step and their draws are related
        // (with libstdc++'s distribution the two ends' jitter values never coincide, with a 2^32
        // engine they coincide three times too often). std::seed_seq scrambles the value first, so
        // neighbouring seeds start unrelated sequences. It also never yields the engine's invalid
        // state 0, so every seed, 0 included, is fine.
        std::seed_seq Mixed {Value};
        m_Engine.seed(Mixed);
    }

    /**
     * @brief Uniform in [Min, Max], both included. Min when Max is not above it.
     * @param Min  The lowest value that can be returned.
     * @param Max  The highest value that can be returned.
     * @return A value in [Min, Max], or Min when Max <= Min.
     */
    uint32_t Value(uint32_t Min, uint32_t Max)
    {
        if (Max <= Min) { return Min; }
        return std::uniform_int_distribution<uint32_t> {Min, Max}(m_Engine);
    }

    /**
     * @brief Base plus a random extra in [0, Base / Divisor), so a Divisor of 4 adds less than a
     * quarter of Base. No extra when Base / Divisor is 0, which includes a Divisor of 0.
     * @param Base     The value to add the extra to.
     * @param Divisor  Sets the largest extra: below Base / Divisor.
     * @return Base, or Base plus the extra.
     */
    uint32_t Jittered(uint32_t Base, uint32_t Divisor)
    {
        const uint32_t Range = Divisor == 0 ? 0 : Base / Divisor;
        return Range == 0 ? Base : Base + Value(0, Range - 1);
    }

private:
    /** @brief The engine the values are drawn from. */
    std::minstd_rand m_Engine;
};
} // namespace ra::turtleford
