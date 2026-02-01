/**
 * @file LoraTransport.cpp
 * @brief Implementation of LoRa transport manager and session classes.
 *
 * This file provides the implementation for LoRa-based communication
 * with session management, message queuing, and radio operations.
 */

#include "Transport/LoraTransport.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>

namespace ra::turtleford
{
bool LoraTransferManager::LoraTransferSession::Send(std::span<const std::byte> Data)
{
    if (!IsOpen()) { return false; }
    auto managerShared = m_Manager.lock();
    return managerShared->Send(Data);
}

std::shared_ptr<ITransferSession> LoraTransferManager::CreateSession(std::unique_ptr<ITransferConfig> Cfg)
{
    if (!Cfg.get()) { return nullptr; }

    std::unique_ptr<LoraTransferConfig> LoraConfigPtr(static_cast<LoraTransferConfig*>(Cfg.release()));
    // Create token and session together so session observes the token immediately
    auto Token     = std::make_shared<LoraTransferSession::SessionToken>();
    // ctor is private friend, can't use std::make_shared
    auto* pSession = new LoraTransferSession(weak_from_this(), std::move(LoraConfigPtr), Token, m_NextSessionId++);
    if (!pSession) { return nullptr; }

    std::shared_ptr<LoraTransferSession> Session {pSession};
    m_Sessions.push_back({Session, std::move(Token)});

    return Session;
}

bool LoraTransferManager::Close(const ITransferSession& Session)
{
    const auto Erased = std::erase_if(m_Sessions, [&](ManagedSession& S) { return S.Session.get() == &Session; });
    return Erased == 1;
}

void LoraTransferManager::Process()
{
    /**
     * @brief Processes queued messages and handles radio operations.
     *
     * This method handles both outgoing and incoming message processing:
     * - Sends queued outgoing messages via the radio
     * - Receives incoming messages from the radio
     * - Broadcasts received data to all active session callbacks
     *
     * Note: Currently assumes single-threaded operation for receive processing.
     *       The static buffer is not thread-safe.
     */
    // no session == no op
    if (m_Sessions.empty()) { return; }

    // send
    if (auto OptMsg = m_OutgoingMessageQueue.Peek())
    {
        const auto& Data = OptMsg->get();
        if (m_Radio->send(reinterpret_cast<const uint8_t*>(Data.data()), Data.size()))
        {
            m_OutgoingMessageQueue.Dequeue();
        }
    }

    // receive, ok for now if this only gets run on 1 thread
    {
        size_t Size      = 0;
        static auto Buff = std::array<uint8_t, 1024>();
        if (!m_Radio->receive(Buff.data(), Buff.max_size(), Size))
        {
            // TODO : Log
            return;
        }

        // broadcast to each session, consider amortizing
        for (auto& M : m_Sessions)
        {
            const auto& pSession      = M.Session;
            const auto& SessionConfig = static_cast<const LoraTransferConfig&>(pSession->Config());

            const auto Ctx =
                TransferContext {.SessionID = 0, .Type = TransportType::Lora, .SourceID = 0, .Session = pSession};
            const std::span<const std::byte> Payload = {reinterpret_cast<const std::byte*>(Buff.data()), Size};

            pSession->Deliver(Ctx, Payload);
        }
    }
}

std::shared_ptr<ITransferSession> LoraTransferManager::GetSession(size_t id) const
{
    for (const auto& M : m_Sessions)
    {
        if (M.Session->ID() == id) { return M.Session; }
    }
    return nullptr;
}
} // namespace ra::turtleford
