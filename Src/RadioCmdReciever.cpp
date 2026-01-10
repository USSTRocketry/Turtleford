#include "RadioCmdReciever.h"
#include "ProtoCodec.h"
//#include "LoraTransport.h"
#include <list>
#include <queue>

enum RadCmd::State state = RadCmd::IDLE;

std::list<std::pair</*impelment enum type to be send or recieve */int,Proto_MainMessage>> recieve_msg_queue = {};

struct Vector3{
    float X;
    float Y;
    float Z;
};

struct FlightData
{
    uint32_t timestamp_ms;
    float bmp_temperature;
    float bmp_pressure;
    float bmp_altitude;
    Vector3 accel;
    Vector3 gyro;
    Vector3 magnetometer;
    float thermometer;
};


void add_work_item_to_send(Proto_MainMessage Msg){
    
}

void (*RecieveData)(Proto_InFlightData) = NULL;

void (*SwitchRadioFrequency)(float) = NULL;

void (*DebugMessage)(std::string*) = NULL;

void add_work_item_to_recieve(std::span<std::byte> Msg){
    auto pMessage = ra::turtleford::ProtoDecode_MainMessage(Msg);
    //schedule_work_item(work_queue_obj/*to be implemented*/, RecieveCmnd, Msg);
}

void add_work_item_to_send(Proto_MainMessage *Msg){
    // coonvert to a byte 
}

//send over the radio
void SendCmnd(void *Msg){
    Proto_MainMessage &Proto_Msg = *static_cast<Proto_MainMessage*>(Msg);
    
    delete &Proto_Msg;
}

// recieve from the radio and do stuff
void RecieveCmnd(Proto_MainMessage Msg){

    switch (Msg.which_message_type)
    {
        case Proto_MainMessage_in_flight_data_tag:
            if (RecieveData != NULL)
            {
                RecieveData(Msg.message_type.in_flight_data);
            }
            break;
        case Proto_MainMessage_switch_radio_frequency_tag:
            if(SwitchRadioFrequency != NULL){
                SwitchRadioFrequency(Msg.message_type.switch_radio_frequency.new_frequency);
            }
            break;
        case Proto_MainMessage_debug_msg_tag:
            if(DebugMessage != NULL){
                
                DebugMessage(static_cast<std::string*>(Msg.message_type.debug_msg.msg.arg));
            }
            break;
        default:
            break;
    }
}