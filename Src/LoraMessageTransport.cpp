#include "LoraTransport.h"

#include "ProtoCodec.h"

namespace
{
} // namespace

namespace ra::turtleford
{
LoraTransferSession::LoraTransferSession() {}

bool LoraTransferSession::Send(std::span<const std::byte> Data) { return false; }
void LoraTransferSession::ReceiveCallback(ITransferSession::ReceiveCallback Cb) {}
} // namespace ra::turtleford
