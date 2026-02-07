#include "Transport/LoraTransport.h"
#include "Transport/TransferSession.h"
#include <chrono>
#include "RadioCmdReciever.h"
#include "ProtoCodec.h"

namespace ra::turtleford
{

void RadioCmndReciever::ManualTimeoutCancelSwitchFrequency(std::chrono::_V2::steady_clock::time_point timeout_time){
    if(std::chrono::steady_clock::now() >= timeout_time){
        state = RadState::READY;
    }
    else{
        //re-add this function to the work queue
    }

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
void RadioCmndReciever::RecieveCommand(const TransferContext& Context, std::span<const std::byte> Data)
{
    const auto Msg = ProtoDecode_MainMessage(Data);

    if(Msg.which_message_type == Proto_MainMessage_switch_radio_frequency_tag || Msg.which_message_type == Proto_MainMessage_ack_tag){
        switch (state)
        {
            case RadState::READY:
                state = RadState::WAITING_FOR_ACK;
                ManualTimeoutCancelSwitchFrequency(std::chrono::steady_clock::now() + std::chrono::seconds(3));
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

RadioCmndReciever::RadioCmndReciever() :
    session(TransferManager.CreateSession(std::make_unique<LoraTransferConfig>(2, 3, RecieveCommand)))
{
}

// send over the radio
void RadioCmndReciever::SendCmnd(Proto_MainMessage msg)
{
    const auto encoded = ProtoEncode(msg);
    if(msg.which_message_type == Proto_MainMessage_switch_radio_frequency_tag){
        state = RadState::WAITING_FOR_ACK;
    }
    TransferManager.Send(encoded);
}

} // namespace ra::turtleford