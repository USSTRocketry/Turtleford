#include "ProtoMain.pb.h"
#include "WorkQueue.h"
#include "Transport/TransferSession.h"
#include <memory>
#include <span>

namespace ra::turtleford
{

class RadioCmndReciever
{
public:
    RadioCmndReciever(ITransferSession* session_in);

    void (*RecieveData)(Proto_InFlightData) = nullptr;

    void (*SwitchRadioFrequency)(float) = nullptr;

    void (*DebugMessage)(std::unique_ptr<std::string>) = nullptr;

    void (*InfoExchange)(Proto_InfoExchange) = nullptr;

    void SendCmnd(Proto_MainMessage msg);

    void SetWorkQueue(hal::WorkQueue* work_queue);
    
    void RecieveCommand(const TransferContext& Context, std::span<const std::byte> Data);

private:
    static void RecieveCommandPassthrough(RadioCmndReciever& reciever, const TransferContext& Context, std::span<const std::byte> Data);

    void ManualTimeoutCancelSwitchFrequency();

    ITransferSession* session;

    hal::WorkQueue* WorkQueue;

    void ThreadRun(Proto_MainMessage Msg);

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
