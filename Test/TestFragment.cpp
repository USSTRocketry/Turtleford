#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "Mocks/FrameMatchers.h"
#include "Transport/Fragment.h"

using namespace ra::turtleford;
using mocks::HasMessageId;
using mocks::IsFragment;
using mocks::IsMarkedBulk;
using mocks::IsMarkedOver;
using testing::AllOf;
using testing::Each;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::Not;
using testing::SizeIs;

namespace
{
constexpr uint8_t Net = 0x42;

std::vector<std::byte> Pattern(size_t Size, uint8_t Start = 0)
{
    std::vector<std::byte> Out(Size);
    for (size_t i = 0; i < Size; ++i) { Out[i] = std::byte {static_cast<uint8_t>(Start + i)}; }
    return Out;
}

std::vector<std::byte> Copy(std::span<const std::byte> Data) { return {Data.begin(), Data.end()}; }

// Cut a message into its frames
std::vector<std::vector<std::byte>> Split(Fragmenter& F, std::span<const std::byte> Message)
{
    std::vector<std::vector<std::byte>> Frames;
    EXPECT_TRUE(F.Begin(Message));
    while (!F.Done())
    {
        Frames.push_back(Copy(F.Current()));
        F.Advance();
    }
    return Frames;
}

// A fragmenter for the network under test, and what it takes to cut a message in two or three
class FragmentTest : public testing::Test
{
protected:
    Fragmenter F {Net, 20}; // 15 payload bytes per frame
};

// The same fragmenter, plus the reassembler on the other end of it
class ReassemblerTest : public FragmentTest
{
protected:
    Reassembler R {Net, 100, 1000};
};
} // namespace

TEST_F(FragmentTest, SmallMessageIsOneFrameWithHeader)
{
    const auto Message = Pattern(10);

    const auto Frames = Split(F, Message);

    ASSERT_THAT(Frames, SizeIs(1));
    EXPECT_THAT(Frames[0], SizeIs(FragmentHeader::Size + 10));
    EXPECT_THAT(Frames[0], IsFragment(Net, 0, 1));
    EXPECT_EQ(Copy(std::span {Frames[0]}.subspan(FragmentHeader::Size)), Message);
}

TEST_F(FragmentTest, LargeMessageIsCutToFrameSize)
{
    const auto Message = Pattern(40);

    const auto Frames = Split(F, Message);

    ASSERT_THAT(Frames, SizeIs(3));
    EXPECT_THAT(Frames, ElementsAre(SizeIs(20), SizeIs(20), SizeIs(FragmentHeader::Size + 10)));
    EXPECT_THAT(Frames, Each(HasMessageId(Frames[0][1]))) << "same message id on every fragment";
    EXPECT_THAT(Frames, ElementsAre(IsFragment(Net, 0, 3), IsFragment(Net, 1, 3), IsFragment(Net, 2, 3)));
}

TEST_F(FragmentTest, ExactMultipleOfFrameSizeHasNoEmptyTail)
{
    EXPECT_THAT(Split(F, Pattern(30)), SizeIs(2));
}

TEST_F(FragmentTest, RejectsEmptyAndTooLargeMessages)
{
    EXPECT_FALSE(F.Begin({}));
    EXPECT_TRUE(F.Done());

    const auto TooLarge = Pattern(F.MaxMessageSize() + 1);
    EXPECT_FALSE(F.Begin(TooLarge));

    const auto Largest = Pattern(F.MaxMessageSize());
    EXPECT_TRUE(F.Begin(Largest));
}

TEST_F(FragmentTest, ConsecutiveMessagesGetDifferentIds)
{
    const auto First  = Split(F, Pattern(5));
    const auto Second = Split(F, Pattern(5));

    EXPECT_THAT(Second, Each(Not(HasMessageId(First[0][1]))));
}

TEST_F(ReassemblerTest, RoundTripsEverySizeUpToTheLimit)
{
    Reassembler Big {Net, 300, 1000};

    for (size_t Size = 1; Size <= 300; ++Size)
    {
        SCOPED_TRACE(testing::Message() << "message of " << Size << " bytes");
        const auto Message = Pattern(Size, static_cast<uint8_t>(Size));
        const auto Frames  = Split(F, Message);

        for (size_t i = 0; i < Frames.size(); ++i)
        {
            const auto Out = Big.Push(Frames[i], 0);
            if (i + 1 < Frames.size()) { ASSERT_THAT(Out, IsEmpty()) << "frame " << i; }
            else { ASSERT_THAT(Copy(Out), ElementsAreArray(Message)); }
        }
    }
    EXPECT_EQ(Big.Dropped(), 0u);
}

TEST_F(ReassemblerTest, IgnoresOtherNetworks)
{
    Fragmenter Stranger {0x99, 20};

    const auto Frames = Split(Stranger, Pattern(5));

    EXPECT_THAT(R.Push(Frames[0], 0), IsEmpty());
    EXPECT_EQ(R.Dropped(), 1u);
}

TEST_F(ReassemblerTest, IgnoresMalformedFrames)
{
    const std::vector<std::byte> HeaderOnly {std::byte {Net}, std::byte {1}, std::byte {0}, std::byte {1}, std::byte {0}};
    const std::vector<std::byte> Truncated {std::byte {Net}, std::byte {1}, std::byte {0}, std::byte {1}};
    const std::vector<std::byte> ZeroCount {
        std::byte {Net}, std::byte {1}, std::byte {0}, std::byte {0}, std::byte {0}, std::byte {7}};
    const std::vector<std::byte> IndexPastCount {
        std::byte {Net}, std::byte {1}, std::byte {2}, std::byte {2}, std::byte {0}, std::byte {7}};

    EXPECT_THAT(R.Push(HeaderOnly, 0), IsEmpty());
    EXPECT_THAT(R.Push(Truncated, 0), IsEmpty());
    EXPECT_THAT(R.Push(ZeroCount, 0), IsEmpty());
    EXPECT_THAT(R.Push(IndexPastCount, 0), IsEmpty());
    EXPECT_THAT(R.Push({}, 0), IsEmpty());
    EXPECT_EQ(R.Dropped(), 5u);
}

TEST_F(ReassemblerTest, MissingFragmentDropsTheWholeMessage)
{
    const auto Frames = Split(F, Pattern(40)); // 3 frames

    EXPECT_THAT(R.Push(Frames[0], 0), IsEmpty());
    // frame 1 lost
    EXPECT_THAT(R.Push(Frames[2], 0), IsEmpty());

    // and a late copy of frame 1 must not resurrect it
    EXPECT_THAT(R.Push(Frames[1], 0), IsEmpty());
    EXPECT_THAT(R.Push(Frames[2], 0), IsEmpty());
}

TEST_F(ReassemblerTest, MessageAfterABrokenOneStillArrives)
{
    const auto Broken = Split(F, Pattern(40));
    const auto Whole  = Pattern(30, 0x80);
    const auto Good   = Split(F, Whole);

    EXPECT_THAT(R.Push(Broken[0], 0), IsEmpty());
    // rest of Broken never arrives; the next message starts over cleanly
    EXPECT_THAT(R.Push(Good[0], 0), IsEmpty());
    EXPECT_THAT(Copy(R.Push(Good[1], 0)), ElementsAreArray(Whole));
}

TEST_F(ReassemblerTest, RepeatedFragmentIsHarmless)
{
    const auto Message = Pattern(40);
    const auto Frames  = Split(F, Message);

    EXPECT_THAT(R.Push(Frames[0], 0), IsEmpty());
    EXPECT_THAT(R.Push(Frames[1], 0), IsEmpty());
    EXPECT_THAT(R.Push(Frames[1], 0), IsEmpty()); // radio delivered it twice
    EXPECT_THAT(Copy(R.Push(Frames[2], 0)), ElementsAreArray(Message));
}

TEST_F(ReassemblerTest, StalePartialMessageTimesOut)
{
    const auto Frames = Split(F, Pattern(40));

    EXPECT_THAT(R.Push(Frames[0], 0), IsEmpty());
    EXPECT_THAT(R.Push(Frames[1], 500), IsEmpty());
    // too long since the previous fragment: the message is abandoned
    EXPECT_THAT(R.Push(Frames[2], 1600), IsEmpty());
    EXPECT_EQ(R.Dropped(), 1u);
}

TEST_F(ReassemblerTest, MessageLargerThanBufferIsDropped)
{
    Reassembler Small {Net, 25, 1000};
    const auto Frames = Split(F, Pattern(40)); // 15 + 15 + 10

    EXPECT_THAT(Small.Push(Frames[0], 0), IsEmpty());
    EXPECT_THAT(Small.Push(Frames[1], 0), IsEmpty()); // 30 > 25
    EXPECT_THAT(Small.Push(Frames[2], 0), IsEmpty());

    // still works afterwards
    const auto Short = Pattern(8);
    const auto Next  = Split(F, Short);
    EXPECT_THAT(Copy(Small.Push(Next[0], 0)), ElementsAreArray(Short));
}

TEST_F(FragmentTest, OnlyTheLastFragmentOfAMarkedMessageCarriesTheOverMark)
{
    const auto Message = Pattern(40);
    ASSERT_TRUE(F.Begin(Message, FragmentHeader::FlagOver));

    std::vector<std::vector<std::byte>> Frames;
    while (!F.Done())
    {
        Frames.push_back(Copy(F.Current()));
        F.Advance();
    }

    ASSERT_THAT(Frames, SizeIs(3));
    EXPECT_THAT(Frames[0], Not(IsMarkedOver()));
    EXPECT_THAT(Frames[1], Not(IsMarkedOver()));
    EXPECT_THAT(Frames[2], IsMarkedOver());
}

TEST_F(FragmentTest, SingleFrameMessageCanBeMarkedOver)
{
    const auto Message = Pattern(5);
    ASSERT_TRUE(F.Begin(Message, FragmentHeader::FlagOver));
    EXPECT_THAT(Copy(F.Current()), IsMarkedOver());
}

TEST_F(FragmentTest, MessageIsNotMarkedUnlessAskedAndTheMarkDoesNotCarryOver)
{
    const auto Marked = Pattern(5);
    ASSERT_TRUE(F.Begin(Marked, FragmentHeader::FlagOver));
    F.Advance();

    EXPECT_THAT(Split(F, Pattern(5)), ElementsAre(Not(IsMarkedOver())));
}

TEST(FragmentHeaderTest, ParseReadsEveryField)
{
    const std::vector<std::byte> Frame {
        std::byte {0x42}, std::byte {7}, std::byte {2}, std::byte {3}, std::byte {FragmentHeader::FlagOver}, std::byte {0xAA}};

    const auto Header = FragmentHeader::Parse(Frame);

    ASSERT_TRUE(Header.has_value());
    EXPECT_EQ(Header->Source, 0x42);
    EXPECT_EQ(Header->MessageId, 7);
    EXPECT_EQ(Header->Index, 2);
    EXPECT_EQ(Header->Count, 3);
    EXPECT_TRUE(Header->IsLast());
    EXPECT_TRUE(Header->IsOver());
}

TEST(FragmentHeaderTest, ParseRejectsAFrameShorterThanTheHeader)
{
    const std::vector<std::byte> Frame(FragmentHeader::Size - 1, std::byte {1});

    EXPECT_FALSE(FragmentHeader::Parse(Frame).has_value());
    EXPECT_FALSE(FragmentHeader::Parse({}).has_value());
}

TEST(FragmentHeaderTest, OverMeansNothingOnAFragmentThatIsNotTheLastOne)
{
    const FragmentHeader Middle {.Source = 1, .MessageId = 1, .Index = 0, .Count = 2, .Flags = FragmentHeader::FlagOver};
    const FragmentHeader Last {.Source = 1, .MessageId = 1, .Index = 1, .Count = 2, .Flags = 0};
    const FragmentHeader Broken {.Source = 1, .MessageId = 1, .Index = 0, .Count = 0, .Flags = FragmentHeader::FlagOver};

    EXPECT_FALSE(Middle.IsOver());
    EXPECT_TRUE(Last.IsLast());
    EXPECT_FALSE(Last.IsOver());
    EXPECT_FALSE(Broken.IsLast());
}

TEST_F(ReassemblerTest, OverMarkRoundTripsAndDoesNotChangeTheMessage)
{
    const auto Message = Pattern(40, 0x10);
    ASSERT_TRUE(F.Begin(Message, FragmentHeader::FlagOver));

    std::vector<FragmentHeader> Headers;
    std::span<const std::byte> Out;
    while (!F.Done())
    {
        const auto Frame = Copy(F.Current());
        Headers.push_back(*FragmentHeader::Parse(Frame));
        Out = R.Push(Frame, 0);
        F.Advance();
    }

    EXPECT_THAT(Copy(Out), ElementsAreArray(Message));
    EXPECT_EQ(R.Dropped(), 0u);
    ASSERT_THAT(Headers, SizeIs(3));
    EXPECT_FALSE(Headers[0].IsOver());
    EXPECT_FALSE(Headers[1].IsOver());
    EXPECT_TRUE(Headers[2].IsOver());
}

TEST_F(FragmentTest, BulkAndOverMarksAreIndependentAndOnlyOnTheLastFragment)
{
    const auto Message = Pattern(40);

    ASSERT_TRUE(F.Begin(Message, FragmentHeader::FlagBulk));
    std::vector<std::vector<std::byte>> Bulk;
    while (!F.Done())
    {
        Bulk.push_back(Copy(F.Current()));
        F.Advance();
    }
    ASSERT_THAT(Bulk, SizeIs(3));
    EXPECT_THAT(Bulk, ElementsAre(AllOf(Not(IsMarkedBulk()), Not(IsMarkedOver())),
                                  AllOf(Not(IsMarkedBulk()), Not(IsMarkedOver())),
                                  AllOf(IsMarkedBulk(), Not(IsMarkedOver()))));

    ASSERT_TRUE(F.Begin(Message, FragmentHeader::FlagBulk | FragmentHeader::FlagOver));
    std::vector<std::vector<std::byte>> Both;
    while (!F.Done())
    {
        Both.push_back(Copy(F.Current()));
        F.Advance();
    }
    EXPECT_THAT(Both.back(), AllOf(IsMarkedBulk(), IsMarkedOver()));
}

TEST(FragmentHeaderTest, BulkRoundTripsThroughParseAndMeansNothingOnAFragmentThatIsNotTheLastOne)
{
    const std::vector<std::byte> Frame {std::byte {1}, std::byte {1}, std::byte {1}, std::byte {2}, std::byte {FragmentHeader::FlagBulk}, std::byte {9}};
    const auto Header = FragmentHeader::Parse(Frame);
    ASSERT_TRUE(Header.has_value());
    EXPECT_TRUE(Header->IsBulk());
    EXPECT_FALSE(Header->IsOver());

    const FragmentHeader Middle {.Source = 1, .MessageId = 1, .Index = 0, .Count = 2, .Flags = FragmentHeader::FlagBulk};
    EXPECT_FALSE(Middle.IsBulk());
}
