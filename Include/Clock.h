/**
 * @file Clock.h
 * @brief Comparisons on a wrapping 32 bit millisecond clock.
 *
 * A millisecond counter such as millis() wraps every 2^32 ms (about 49 days). Comparing two readings
 * with < or - directly breaks at the wrap; these functions compare them as a signed distance
 * instead, which stays right while the two readings are less than 2^31 ms (about 24 days) apart.
 */

#pragma once

#include <cstdint>

namespace ra::turtleford
{
/**
 * @brief Now is at or past Deadline, correct across the 49 day wrap of a millisecond counter.
 * @param NowMs       The current reading of the millisecond clock.
 * @param DeadlineMs  The reading to compare against.
 * @return true when NowMs is equal to or later than DeadlineMs.
 */
constexpr bool TimeReached(uint32_t NowMs, uint32_t DeadlineMs) { return static_cast<int32_t>(NowMs - DeadlineMs) >= 0; }

/**
 * @brief Milliseconds from Since to Now, correct across the 49 day wrap of a millisecond counter.
 * @param NowMs    The current reading of the millisecond clock.
 * @param SinceMs  An earlier reading of the same clock.
 * @return The milliseconds that passed between the two readings.
 */
constexpr uint32_t Elapsed(uint32_t NowMs, uint32_t SinceMs) { return NowMs - SinceMs; }
} // namespace ra::turtleford
