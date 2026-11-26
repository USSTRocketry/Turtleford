#pragma once

// Matchers for the frames a LoraTransport puts on air: [Source][MessageId][Index][Count][Flags][payload...]
// They accept a std::span or a std::vector of bytes, so the same matcher serves an argument of
// ITelemetryRadio::send() and an element of FakeTelemetryRadio::Sent.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include "Transport/Fragment.h"

namespace mocks
{
/** @brief The frame is fragment Index of Count sent by Source. */
MATCHER_P3(IsFragment,
           Source,
           Index,
           Count,
           std::string(negation ? "is not" : "is") + " fragment " + testing::PrintToString(static_cast<int>(Index)) +
               " of " + testing::PrintToString(static_cast<int>(Count)) + " from address " +
               testing::PrintToString(static_cast<int>(Source)))
{
    if (arg.size() < ra::turtleford::FragmentHeader::Size)
    {
        *result_listener << "is only " << arg.size() << " bytes, shorter than a header";
        return false;
    }

    // the frame may be bytes or std::byte
    const auto At = [&](size_t i) { return static_cast<uint8_t>(arg[i]); };

    *result_listener << "is fragment " << static_cast<int>(At(2)) << " of " << static_cast<int>(At(3))
                     << " from address " << static_cast<int>(At(0));
    return At(0) == static_cast<uint8_t>(Source) && At(2) == static_cast<uint8_t>(Index) &&
           At(3) == static_cast<uint8_t>(Count);
}

/** @brief The frame carries the "over" mark: its sender listens after this message. */
MATCHER(IsMarkedOver, std::string(negation ? "is not" : "is") + " marked over")
{
    return arg.size() >= ra::turtleford::FragmentHeader::Size &&
           (static_cast<uint8_t>(arg[4]) & ra::turtleford::FragmentHeader::FlagOver) != 0;
}

/** @brief The frame carries the "bulk" mark: it is one of a stream. */
MATCHER(IsMarkedBulk, std::string(negation ? "is not" : "is") + " marked bulk")
{
    return arg.size() >= ra::turtleford::FragmentHeader::Size &&
           (static_cast<uint8_t>(arg[4]) & ra::turtleford::FragmentHeader::FlagBulk) != 0;
}

/** @brief The frame leads with the sender address Source. */
MATCHER_P(HasSource, Source, std::string(negation ? "is not" : "is") + " from address " +
                                 testing::PrintToString(static_cast<int>(Source)))
{
    return !arg.empty() && static_cast<uint8_t>(arg[0]) == static_cast<uint8_t>(Source);
}

/** @brief The frame belongs to the message with this id (a uint8_t or std::byte). */
MATCHER_P(HasMessageId, Id, std::string(negation ? "does not belong" : "belongs") + " to message " +
                                testing::PrintToString(static_cast<int>(Id)))
{
    return arg.size() > 1 && static_cast<uint8_t>(arg[1]) == static_cast<uint8_t>(Id);
}

/** @brief What follows the fragment header equals Expected (a std::vector<std::byte>). */
MATCHER_P(CarriesPayload, Expected, "carries the expected payload")
{
    constexpr size_t Header = ra::turtleford::FragmentHeader::Size;
    if (arg.size() < Header)
    {
        *result_listener << "is only " << arg.size() << " bytes, shorter than a header";
        return false;
    }

    const size_t Size = arg.size() - Header;
    if (Size != Expected.size())
    {
        *result_listener << "has " << Size << " payload bytes, expected " << Expected.size();
        return false;
    }
    for (size_t i = 0; i < Size; ++i)
    {
        if (static_cast<std::byte>(arg[Header + i]) != Expected[i])
        {
            *result_listener << "differs at payload byte " << i;
            return false;
        }
    }
    return true;
}

/** @brief Fragment Index of Count from Source carrying exactly Payload. */
template <typename Payload>
auto FrameIs(uint8_t Source, size_t Index, size_t Count, const Payload& Bytes)
{
    return testing::AllOf(IsFragment(Source, Index, Count), CarriesPayload(Bytes));
}
} // namespace mocks
