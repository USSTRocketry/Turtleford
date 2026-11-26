// Tests of the simulated LoRa air and radio in Sim/SimRadio.h itself: airtime, blocking send, half
// duplex, collisions, the one frame receive buffer, loss and addressing. They are kept apart from
// the stack tests (TestRadioOverLora.cpp) so that a failure there can be trusted to be about the
// stack and not the simulation.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "Sim/SimRadio.h"

using sim::RadioAir;
using sim::SimRadio;
using testing::Optional;
using testing::SizeIs;

namespace
{
std::vector<uint8_t> Bytes(size_t Count, uint8_t Fill = 0x55) { return std::vector<uint8_t>(Count, Fill); }

// What a radio has received, if anything. nullopt is silence.
std::optional<std::vector<uint8_t>> Heard(SimRadio& Radio)
{
    std::array<uint8_t, 255> Buffer {};
    size_t Length = 0;
    if (!Radio.receive(Buffer, Length)) { return std::nullopt; }
    return std::vector<uint8_t>(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Length));
}

// Two bare radios on one air, a clock the test moves by hand
class SimRadioTest : public testing::Test
{
protected:
    SimRadioTest()
    {
        A.setFrequency(915.0f);
        B.setFrequency(915.0f);
    }

    // The calling radio acts at time Ms
    void At(uint32_t Ms) { Air.SetNow(Ms); }

    RadioAir Air;
    SimRadio A {Air};
    SimRadio B {Air};
};
} // namespace

TEST_F(SimRadioTest, FrameTakesOverheadPlusPerByteOnAirAndSendBlocksForIt)
{
    At(100);

    EXPECT_TRUE(A.send(Bytes(100)));

    EXPECT_EQ(Air.AirtimeMs(100), 60u);
    EXPECT_EQ(Air.Now(), 160u) << "send() must return only once the frame is off the air";
}

TEST_F(SimRadioTest, FrameIsHeardOnlyOnceItHasFinished)
{
    At(0);
    A.send(Bytes(100));

    At(59);
    EXPECT_EQ(Heard(B), std::nullopt);
    At(60);
    EXPECT_THAT(Heard(B), Optional(Bytes(100)));
    EXPECT_EQ(Heard(B), std::nullopt) << "taken once";
}

TEST_F(SimRadioTest, TwoFramesOverlappingInTimeAreBothLost)
{
    At(0);
    A.send(Bytes(100)); // on air 0..60
    At(30);
    B.send(Bytes(100)); // on air 30..90

    At(500);
    EXPECT_EQ(Heard(A), std::nullopt);
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(Air.Stats.LostCollision, 2u);
}

TEST_F(SimRadioTest, FramesBackToBackDoNotCollide)
{
    A.DeafAfterTransmit = false; // B's frame starts the moment A's ends
    At(0);
    A.send(Bytes(100)); // 0..60
    At(60);
    B.send(Bytes(100)); // 60..120

    At(500);
    EXPECT_NE(Heard(A), std::nullopt);
    EXPECT_NE(Heard(B), std::nullopt);
    EXPECT_EQ(Air.Stats.LostCollision, 0u);
}

TEST_F(SimRadioTest, FramesOnDifferentFrequenciesDoNotCollide)
{
    SimRadio C {Air};
    SimRadio D {Air};
    C.setFrequency(916.0f);
    D.setFrequency(916.0f);

    At(0);
    A.send(Bytes(100));
    At(10);
    C.send(Bytes(100));

    At(500);
    EXPECT_NE(Heard(B), std::nullopt);
    EXPECT_NE(Heard(D), std::nullopt);
    EXPECT_EQ(Air.Stats.LostCollision, 0u);
}

TEST_F(SimRadioTest, RadioHearsNothingWhileItTransmitsItself)
{
    B.DeafAfterTransmit = false; // so only the transmission itself can explain a loss

    At(0);
    A.send(Bytes(100)); // on air 0..60

    // B transmits 10..21 on another frequency (so it does not collide with A) and is back on 915 before A ends
    At(10);
    B.setFrequency(916.0f);
    B.send(Bytes(2));
    B.setFrequency(915.0f);

    At(500);
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(B.Stats.LostTransmitting, 1u);
    EXPECT_EQ(B.Stats.LostDeaf, 0u);
    EXPECT_EQ(Air.Stats.LostCollision, 0u);
}

TEST_F(SimRadioTest, SingleFrameBufferLosesTheSecondFrameAndCountsAnOverrun)
{
    At(0);
    A.send(Bytes(10, 1)); // ends 15
    At(15);
    A.send(Bytes(10, 2)); // ends 30
    At(30);
    A.send(Bytes(10, 3)); // ends 45

    At(100); // B has not looked in the meantime
    EXPECT_THAT(Heard(B), Optional(Bytes(10, 1))) << "the first one stays, the newer ones are lost";
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(B.Stats.Overruns, 2u);
    EXPECT_EQ(Air.Stats.LostOverrun, 2u);
}

TEST_F(SimRadioTest, ReceivingBetweenFramesAvoidsTheOverrun)
{
    At(0);
    A.send(Bytes(10, 1)); // ends 15
    At(15);
    EXPECT_NE(Heard(B), std::nullopt);
    A.send(Bytes(10, 2)); // ends 30
    At(30);
    EXPECT_NE(Heard(B), std::nullopt);
    EXPECT_EQ(B.Stats.Overruns, 0u);
}

TEST_F(SimRadioTest, RadioIsDeafAfterTransmitUntilItCallsReceive)
{
    At(0);
    B.send(Bytes(10)); // B on air 0..15, then deaf
    At(15);
    A.send(Bytes(10)); // 15..30, B has not called receive() since its send

    At(100);
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(B.Stats.LostDeaf, 1u);

    // once B has called receive() it listens again
    At(100);
    A.send(Bytes(10));
    At(200);
    EXPECT_NE(Heard(B), std::nullopt);
}

TEST_F(SimRadioTest, DeafAfterTransmitCanBeSwitchedOff)
{
    B.DeafAfterTransmit = false;
    At(0);
    B.send(Bytes(10)); // 0..15
    At(15);
    A.send(Bytes(10)); // 15..30

    At(100);
    EXPECT_NE(Heard(B), std::nullopt);
    EXPECT_EQ(B.Stats.LostDeaf, 0u);
}

TEST_F(SimRadioTest, FrameIsHeardOnlyByRadiosOnTheSameFrequency)
{
    B.setFrequency(916.0f);

    At(0);
    A.send(Bytes(10));
    At(100);
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(Air.Stats.LostWrongFrequency, 1u);

    B.setFrequency(915.0f);
    A.send(Bytes(10));
    At(200);
    EXPECT_NE(Heard(B), std::nullopt);
}

TEST_F(SimRadioTest, CutSwitchSilencesTheAirButFramesStillTakeTime)
{
    Air.Cut = true;
    At(0);
    A.send(Bytes(100));

    EXPECT_EQ(Air.Now(), 60u);
    At(100);
    EXPECT_EQ(Heard(B), std::nullopt);
    EXPECT_EQ(Air.Stats.LostCut, 1u);
}

TEST_F(SimRadioTest, RandomLossIsSeededSoItRepeats)
{
    const auto Replay = [](uint32_t Seed)
    {
        sim::AirConfig Config {};
        Config.Seed = Seed;
        RadioAir LossyAir {Config};
        SimRadio Tx {LossyAir};
        SimRadio Rx {LossyAir};
        LossyAir.LossPercent = 50;

        uint32_t Pattern = 0;
        for (uint32_t i = 0; i < 20; ++i)
        {
            LossyAir.SetNow(i * 100);
            Tx.send(Bytes(10));
            LossyAir.SetNow(i * 100 + 50);
            Pattern = (Pattern << 1) | (Heard(Rx) ? 1u : 0u);
        }
        return Pattern;
    };

    EXPECT_EQ(Replay(7), Replay(7));
    EXPECT_NE(Replay(7), 0u);
    EXPECT_NE(Replay(7), 0xFFFFFu);
}

TEST_F(SimRadioTest, DropFilterSeesTheFrameAndTheSender)
{
    size_t SeenSender = 99;
    Air.Drop = [&](std::span<const uint8_t> Frame, size_t Sender)
    {
        SeenSender = Sender;
        return Frame.size() == 7;
    };

    At(0);
    B.send(Bytes(7));
    At(50);
    B.send(Bytes(8));
    At(100);

    EXPECT_EQ(SeenSender, B.Index());
    EXPECT_THAT(Heard(A), Optional(SizeIs(8)));
    EXPECT_EQ(Air.Stats.LostFiltered, 1u);
}

TEST_F(SimRadioTest, FrameLargerThanTheDriverLimitIsRefused)
{
    EXPECT_FALSE(A.send(Bytes(SimRadio::MaxPayload + 1)));
    EXPECT_TRUE(A.send(Bytes(SimRadio::MaxPayload)));
}

TEST_F(SimRadioTest, FrameIsKeptOnlyByTheRadioItIsAddressedTo)
{
    SimRadio Sender {Air};
    SimRadio Wanted {Air};
    SimRadio Other {Air};
    SimRadio Snoop {Air};
    for (SimRadio* R : {&Sender, &Wanted, &Other, &Snoop}) { R->setFrequency(915.0f); }

    Sender.setDestinationAddress(7);
    Wanted.setAddress(7);
    Other.setAddress(8);
    Snoop.setAddress(9);
    Snoop.setPromiscuousMode(true);

    const std::array<uint8_t, 5> Frame {1, 2, 3, 4, 5};
    ASSERT_TRUE(Sender.send(Frame));

    std::array<uint8_t, 16> Buffer {};
    size_t Size = 0;
    EXPECT_TRUE(Wanted.receive(Buffer, Size));
    EXPECT_FALSE(Other.receive(Buffer, Size));
    EXPECT_TRUE(Snoop.receive(Buffer, Size)) << "promiscuous hears everything";
    EXPECT_EQ(Other.Stats.LostWrongAddress, 1u);
    EXPECT_EQ(Air.Stats.LostWrongAddress, 1u);
}
