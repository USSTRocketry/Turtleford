#include "ProtoMain.pb.h"
#include "WorkQueue.h"
#include "RadioCmdReciever.h"
#include "Transport/LoraTransport.h"
#include "Transport/TransferSession.h"

namespace ra::turtleford
{

class TurtlefordMain
{
    public:
        TurtlefordMain();

        void Update();

        void SetRecieveDataCallback(void (*RecieveData)(Proto_InFlightData));

        void SetDebugMsgCallback(void (*DebugMessage)(std::unique_ptr<std::string>));

        void SetInfoExchangeCallback(void (*InfoExchange)(Proto_InfoExchange));
    
    private:
        hal::WorkQueue WorkQueue;

        RadioCmndReciever CommandSenderReciever;

        LoraTransferManager TransferManager {nullptr};


};

};