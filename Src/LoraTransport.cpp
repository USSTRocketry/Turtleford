#include "Transport/LoraTransport.h"

#include <array>

namespace ra::turtleford
{
bool LoraTransferSession::Send(std::span<const std::byte> Data)
{
    if (m_Manager.expired()) { return false; }
    auto Manager = m_Manager.lock();
    return Manager->Send(Data);
}

std::shared_ptr<ITransferSession> LoraTransferManager::CreateSession(std::unique_ptr<ITransferConfig> Cfg)
{
    std::unique_ptr<LoraTransferConfig> LoraConfigPtr(static_cast<LoraTransferConfig*>(Cfg.release()));

    auto Session = std::make_shared<LoraTransferSession>(weak_from_this(), std::move(LoraConfigPtr));
    m_Sessions.push_back(Session);
    return Session;
}

void LoraTransferManager::Process()
{
    // send
    if (m_OutgoingMessageQueue.Size() > 0)
    {
        if (m_Radio->send(m_OutgoingMessageQueue.Peek().value())) { m_OutgoingMessageQueue.Dequeue(); }
    }

    // receive, ok for now if this only gets run on 1 thread
    {
        static auto Buff = std::array<std::byte, 1024>();
        size_t Size      = Buff.max_size();
        if (!m_Radio->send(Buff.data(), Size))
        {
            // TODO : Log
        }
        auto& pSession            = m_Sessions.front();
        const auto& SessionConfig = static_cast<const LoraTransferConfig&>(pSession->Config());

        SessionConfig.m_Callback({.SessionID = 0, .Type = TransportType::Lora, .SourceID = 0, .Session = pSession},
                                 {Buff.data(), Size});
    }
}

void LoraTransferManager::ProcessIncomingMessage() {}

std::vector<std::shared_ptr<ITransferSession>> LoraTransferManager::ActiveSessions() const
{
    return {m_Sessions.cbegin(), m_Sessions.cend()};
}
} // namespace ra::turtleford
