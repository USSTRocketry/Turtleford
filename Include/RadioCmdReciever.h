#pragma once

#include "ProtoMain.pb.h"
#include "WorkQueue.h"
#include "Transport/TransferSession.h"
#include <memory>
#include <span>
#include <optional>

namespace ra::turtleford
{

class RadioCmndReciever
{
public:
    RadioCmndReciever(std::shared_ptr<ITransferSession> session_in, float initial_radio_frequency);

    std::function<void(Proto_InFlightData)> RecieveData;

    std::function<void(float)> SwitchRadioFrequency;

    std::function<void(std::unique_ptr<std::string>)> DebugMessage;

    std::function<void(Proto_InfoExchange)> InfoExchange;

    void SendCmnd(Proto_MainMessage msg);

    void SetWorkQueue(hal::WorkQueue* work_queue);

    void RecieveCommand(const TransferContext& Context, std::span<const std::byte> Data);

    void ManualTimeoutCancelSwitchFrequency();

    void ThreadRun(Proto_MainMessage Msg);

    struct ThreadRunStruct
    {
        RadioCmndReciever* radioCmndReciever;
        Proto_MainMessage msg;
    };
    

private:
    static void RecieveCommandPassthrough(RadioCmndReciever& reciever,
                                          const TransferContext& Context,
                                          std::span<const std::byte> Data);

    std::shared_ptr<ITransferSession> session;

    hal::WorkQueue* WorkQueue;

    hal::WorkQueue::WorkHandle ManualTimeoutWorkHandle {}; 

    float newRadioFrequency = 0.0;
    float oldRadioFrequency = 0.0;

    enum class RadState
    {
        READY,
        WAITING_FOR_ACK,
        SWITCHING_FREQUENCY,
        WAITING_FOR_ACK_ON_NEW_FREQUENCY
    };

    RadState state = RadState::READY;
};

} // namespace ra::turtleford
