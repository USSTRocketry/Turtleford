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
    if (!IsOpen()) [[unlikely]] { return false; }

    auto Mgr = m_Manager.lock();
    return Mgr->Send(m_SessionId, Data);
}

void LoraTransferManager::LoraTransferSession::RegisterCallback(ITransferSession::ReceiveCallback Cb)
{
    if (auto Mgr = m_Manager.lock()) { Mgr->SetCallbackForSession(m_SessionId, std::move(Cb)); }
}

bool LoraTransferManager::LoraTransferSession::IsOpen()
{
    if (auto Mgr = m_Manager.lock()) { return Mgr->IsSessionAlive(m_SessionId); }
    return false;
}

size_t LoraTransferManager::LoraTransferSession::ID() const { return m_SessionId; }

const ITransferConfig& LoraTransferManager::LoraTransferSession::Config() const
{
    if (auto Mgr = m_Manager.lock()) [[likely]]
    {
        if (auto Ptr = Mgr->GetConfig(m_SessionId)) { return *Ptr; }
    }

    // we should never hit this case
    static LoraTransferConfig InvalidConfig {0, 0};
    return InvalidConfig;
}

std::shared_ptr<ITransferSession> LoraTransferManager::CreateSession(std::unique_ptr<ITransferConfig> Cfg)
{
    if (!Cfg.get()) { return nullptr; }

    std::unique_ptr<LoraTransferConfig> LoraConfigPtr(static_cast<LoraTransferConfig*>(Cfg.release()));
    auto* pSession = new LoraTransferSession(weak_from_this(), m_NextSessionId++);
    if (!pSession) { return nullptr; }

    std::shared_ptr<LoraTransferSession> Session {pSession};
    // ManagedSession owns the per-session state (config + callback)
    m_Sessions.push_back({Session, {}, std::move(LoraConfigPtr)});

    return Session;
}
bool LoraTransferManager::Close(const ITransferSession& Session)
{
    const auto Erased = std::erase_if(m_Sessions, [&](ManagedSession& S) { return S.Session.get() == &Session; });
    return Erased == 1;
}

void LoraTransferManager::SetCallbackForSession(size_t Id, ITransferSession::ReceiveCallback Cb)
{
    for (auto& M : m_Sessions)
    {
        if (M.Session->ID() == Id)
        {
            M.Callback = std::move(Cb);
            return;
        }
    }
}

bool LoraTransferManager::IsSessionAlive(size_t Id) const
{
    // A session is considered alive if it is currently present in the managed list.
    for (const auto& M : m_Sessions)
    {
        if (M.Session->ID() == Id) { return true; }
    }
    return false;
}

const ITransferConfig* LoraTransferManager::GetConfig(size_t Id) const
{
    for (const auto& M : m_Sessions)
    {
        if (M.Session->ID() == Id) { return M.Config.get(); }
    }
    return nullptr;
}

bool LoraTransferManager::Send(size_t SessionId, std::span<const std::byte> Data)
{
    // the HAL is currently not thread safe, so all send must be synchronized through manager
    if (Data.size() > LoraMTU) { return false; }

    const auto* pCfg = GetConfig(SessionId);
    if (!pCfg) [[unlikely]] { return false; }

    const auto& Cfg = static_cast<const LoraTransferConfig&>(*pCfg);

    OutgoingMessage Msg {
        .DestinationId = Cfg.Addr.SendTo,
        .SourceId      = Cfg.Addr.SelfId,
        .Length        = static_cast<decltype(Msg.Length)>(Data.size()),
        .Data          = {},
    };
    std::copy_n(Data.begin(), Msg.Length, Msg.Data.begin());

    return m_OutgoingMessageQueue.Queue(std::move(Msg));
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
        const auto& Msg = OptMsg->get();

        if (m_Radio->send({reinterpret_cast<const uint8_t*>(Msg.Data.data()), Msg.Length}))
        {
            m_OutgoingMessageQueue.Dequeue();
        }
    }

    // receive, ok for now if this only gets run on 1 thread
    {
        size_t Size      = 0;
        static auto Buff = std::array<uint8_t, 1024>();
        if (!m_Radio->receive(std::span {Buff}, Size))
        {
            // TODO : Log
            return;
        }

        if (Size < 1) { return; }

        // broadcast to each session
        // we currently cannot distinguish traffic sources without packet headers,
        // so we must notify all potentital listeners.
        for (auto& M : m_Sessions)
        {
            if (M.Callback)
            {
                // We assume the message comes from the session's configured peer for now
                // since we lack source identification in the raw packet.
                const auto Ctx                           = TransferContext {.SessionID = M.Session->ID(),
                                                                            .Type      = TransportType::Lora,
                                                                            .SourceID  = M.Config->Addr.RecvFrom,
                                                                            .Session   = M.Session};
                const std::span<const std::byte> Payload = {reinterpret_cast<const std::byte*>(Buff.data()), Size};

                M.Callback(Ctx, Payload);
            }
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
