#include "Transport/LoraTransport.h"
#include "Transport/TransferSession.h"
#include "RadioCmdReciever.h"
#include "ProtoCodec.h"
#include "WorkQueue.h"
#include "Log.h"

namespace ra::turtleford
{

// A timeout that once the timer expires, switches radio state to ready and cancels the frequency switch
void RadioCmndReciever::ManualTimeoutCancelSwitchFrequency(){
    state = RadState::READY;
}

void RadioCmndReciever::ThreadRun(Proto_MainMessage Msg)
{
    switch (Msg.which_message_type)
    {
        case Proto_MainMessage_in_flight_data_tag:
            if (RecieveData != nullptr) { RecieveData(Msg.message_type.in_flight_data); }
            break;
        case Proto_MainMessage_debug_msg_tag:
            if (DebugMessage != nullptr)
            {
                DebugMessage(
                    std::unique_ptr<std::string>(static_cast<std::string*>(Msg.message_type.debug_msg.msg.arg)));
            }
            break;
        case Proto_MainMessage_info_exchange_tag:
            if (InfoExchange != nullptr) { InfoExchange(Msg.message_type.info_exchange);}
            break;
        default:
            break;
    }
}

// recieves a command from the radio and decodes it using the message type to determine what it will do
void RadioCmndReciever::RecieveCommand(const TransferContext& Context, std::span<const std::byte> Data)
{
    const auto MsgOption = ProtoDecode_MainMessage(Data);

    if(!MsgOption.has_value()){
        // THIS IS AN ERROR
        
        return;
    }

    const auto Msg = MsgOption.value();
    

    if(Msg.which_message_type == Proto_MainMessage_switch_radio_frequency_tag || Msg.which_message_type == Proto_MainMessage_ack_tag){
        switch (state)
        {
            case RadState::READY:
                state = RadState::WAITING_FOR_ACK;
                // ADD TO Work Queue
                // ManualTimeoutCancelSwitchFrequency();
                SendCmnd(PbGen_AckMsg(Proto_MainMessage_switch_radio_frequency_tag));
                
                break;
            case RadState::WAITING_FOR_ACK:
                if (Msg.which_message_type == Proto_MainMessage_ack_tag &&
                    Msg.message_type.ack.response_to_which_message == Proto_MainMessage_switch_radio_frequency_tag)
                {
                    //SWITCH FREQUENCY HERE
                    // SEND ACK BACK HERE
                    SendCmnd(PbGen_AckMsg(Proto_MainMessage_switch_radio_frequency_tag));
                    state = RadState::SWITCHING_FREQUENCY;
                }

                break;
            case RadState::SWITCHING_FREQUENCY:
                if (Msg.which_message_type == Proto_MainMessage_ack_tag)
                {
                    state = RadState::READY;
                }else{
                    ThreadRun(Msg);
                }
                break;
            default:
                break;
        }        
    }
    else{
        ThreadRun(Msg);
    }
    
}

void RadioCmndReciever::SetWorkQueue(hal::WorkQueue* work_queue){
    WorkQueue = work_queue;
}


RadioCmndReciever::RadioCmndReciever(ITransferSession* session_in)
{
    session = session_in;
    //insane c++ magic (lambda func)
    session->RegisterCallback([&](auto ...arg){RecieveCommand(arg...);});
    
}

// send over the radio
void RadioCmndReciever::SendCmnd(Proto_MainMessage msg)
{
    const auto encoded = ProtoEncode(msg);
    if(msg.which_message_type == Proto_MainMessage_switch_radio_frequency_tag){
        state = RadState::WAITING_FOR_ACK;
    }
    session->Send(encoded);
}

} // namespace ra::turtleford
