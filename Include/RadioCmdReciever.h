#include "ProtoMain.pb.h"
#include <queue>
#include <memory>
#include <span>

namespace ra::turtleford
{

class RadioCmndReciever
{
public:
    RadioCmndReciever();

    void (*RecieveData)(Proto_InFlightData) = nullptr;

    void (*SwitchRadioFrequency)(float) = nullptr;

    void (*DebugMessage)(std::unique_ptr<std::string>) = nullptr;

    void (*InfoExchange)(Proto_InfoExchange) = nullptr;

    void SendCmnd(Proto_MainMessage msg);

    void ProcessWorkQueue();
    
    void RecieveCommand(const TransferContext& Context, std::span<const std::byte> Data);

private:
    static void RecieveCommandPassthrough(RadioCmndReciever& reciever, const TransferContext& Context, std::span<const std::byte> Data);

    void ManualTimeoutCancelSwitchFrequency(std::chrono::_V2::steady_clock::time_point timeout_time);

    LoraTransferManager TransferManager {nullptr};

    std::shared_ptr<ITransferSession> session;

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