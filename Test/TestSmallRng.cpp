#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include "SmallRng.h"

using ra::turtleford::SmallRng;
using testing::AllOf;
using testing::Each;
using testing::Ge;
using testing::Le;
using testing::SizeIs;

namespace
{
std::vector<uint32_t> Draw(SmallRng& From, uint32_t Min, uint32_t Max, size_t Count)
{
    std::vector<uint32_t> Out(Count);
    for (auto& Value : Out) { Value = From.Value(Min, Max); }
    return Out;
}
} // namespace

TEST(SmallRngTest, ValueStaysInsideTheRangeBothEndsIncluded)
{
    SmallRng Rng {7};

    EXPECT_THAT(Draw(Rng, 0, 9, 1000), Each(Le(9u)));
    EXPECT_THAT(Draw(Rng, 100, 137, 1000), Each(AllOf(Ge(100u), Le(137u))));
    EXPECT_THAT(Draw(Rng, 1, 0x3FFF, 1000), Each(AllOf(Ge(1u), Le(0x3FFFu))));
    EXPECT_THAT(Draw(Rng, 0, std::numeric_limits<uint32_t>::max(), 1000), SizeIs(1000));
}

TEST(SmallRngTest, EveryValueOfASmallRangeTurnsUp)
{
    SmallRng Rng {7};
    const auto Values = Draw(Rng, 3, 12, 1000);

    EXPECT_THAT(std::set<uint32_t>(Values.begin(), Values.end()), SizeIs(10));
}

TEST(SmallRngTest, RangeOfOneValueOrTheWrongWayRoundGivesMin)
{
    SmallRng Rng {7};

    EXPECT_THAT(Draw(Rng, 5, 5, 100), Each(5u));
    EXPECT_THAT(Draw(Rng, 5, 2, 100), Each(5u));
}

TEST(SmallRngTest, JitteredAddsAnExtraBelowBaseOverDivisorAndUsesTheWholeRange)
{
    SmallRng Rng {7};
    std::set<uint32_t> Seen;
    for (int i = 0; i < 1000; ++i) { Seen.insert(Rng.Jittered(100, 4)); }

    EXPECT_THAT(Seen, SizeIs(25)); // 100 .. 124, not 125
    EXPECT_EQ(*Seen.begin(), 100u);
    EXPECT_EQ(*Seen.rbegin(), 124u);
}

TEST(SmallRngTest, JitteredWithADivisorOfZeroAddsNothing)
{
    SmallRng Rng {7};

    for (int i = 0; i < 100; ++i) { EXPECT_EQ(Rng.Jittered(100, 0), 100u); }
}

TEST(SmallRngTest, JitteredWithABaseSmallerThanTheDivisorAddsNothing)
{
    SmallRng Rng {7};

    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(Rng.Jittered(3, 4), 3u);
        EXPECT_EQ(Rng.Jittered(0, 4), 0u);
    }
}

TEST(SmallRngTest, TwoEnginesWithTheSameSeedGiveTheSameSequence)
{
    SmallRng Mine {1234};
    SmallRng Theirs {1234};

    EXPECT_EQ(Draw(Mine, 0, 999, 100), Draw(Theirs, 0, 999, 100));
}

TEST(SmallRngTest, SeedStartsTheSequenceAgain)
{
    SmallRng Rng {1234};
    const auto First = Draw(Rng, 0, 999, 100);

    Rng.Seed(1234);
    EXPECT_EQ(Draw(Rng, 0, 999, 100), First);
}

// The seeds in use are small and close together: the two ends' Seed*2 and Seed*2+1, device
// addresses 1 and 2, consecutive boot counters. Engines seeded with neighbours must draw unrelated
// values, or the two ends keep picking related waits. Over many neighbouring pairs, how often their
// draws are equal and how they correlate must be what independent draws give. Without the seed
// sequence this fails with libstdc++, where the two ends' draws are never equal.
TEST(SmallRngTest, NeighbouringSeedsDrawUnrelatedValues)
{
    constexpr uint32_t Pairs = 4000;
    constexpr uint32_t Draws = 100;
    constexpr uint32_t Range = 400;

    double SumX = 0, SumY = 0, SumXY = 0, SumXX = 0, SumYY = 0;
    uint32_t Equal = 0;
    for (uint32_t Seed = 0; Seed < Pairs; ++Seed)
    {
        SmallRng Mine {Seed};
        SmallRng Theirs {Seed + 1};
        for (uint32_t i = 0; i < Draws; ++i)
        {
            const double X = Mine.Value(0, Range - 1);
            const double Y = Theirs.Value(0, Range - 1);
            SumX += X;
            SumY += Y;
            SumXY += X * Y;
            SumXX += X * X;
            SumYY += Y * Y;
            if (X == Y) { ++Equal; }
        }
    }

    // 400,000 pairs of draws: independent ones are equal 1 time in 400 (about 1000, give or take
    // 32) and their correlation is within about 0.0016 of 0. The margins are several times that.
    const double N    = static_cast<double>(Pairs) * Draws;
    const double Corr = (SumXY - SumX * SumY / N) / std::sqrt((SumXX - SumX * SumX / N) * (SumYY - SumY * SumY / N));
    EXPECT_NEAR(Equal / N, 1.0 / Range, 0.0006);
    EXPECT_LT(std::abs(Corr), 0.01);
}
