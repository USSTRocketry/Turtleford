#pragma once

#include <span>
#include <vector>

#include "TransferSession.h"
#include "CircularBuffer.h"
// #include "Avionics_HAL.h"

namespace ra::turtleford
{
// Pipe info
struct LoraTransferConfig final : public ITransferConfig
{
    struct
    {
        uint8_t RecvFrom;
        uint8_t SendTo;
    } Addr;

    std::unique_ptr<ITransferConfig> Clone() const override { return std::make_unique<LoraTransferConfig>(*this); }
    ITransferSession::ReceiveCallback m_Callback {};

    LoraTransferConfig(const LoraTransferConfig&)            = default;
    LoraTransferConfig& operator=(const LoraTransferConfig&) = default;
};

// Pipe abstraction
class LoraTransferSession final : public ITransferSession
{
public:
    bool Send(std::span<const std::byte> Data) override;
    void RegisterCallback(ReceiveCallback Cb) override { m_Config->m_Callback = Cb; }
    void Close() override {};

    bool IsOpen() override { return !m_Manager.expired(); };
    size_t ID() const override { return m_ID; }
    TransportType Transport() const override { return TransportType::Lora; };
    const ITransferConfig& Config() const override { return *m_Config; };

public:
    LoraTransferSession(std::weak_ptr<class LoraTransferManager> Manager, std::unique_ptr<LoraTransferConfig> Cfg) :
        m_Config(std::move(Cfg)), m_Manager(std::move(Manager))
    {
    }
    ~LoraTransferSession();

private:
    size_t m_ID {};
    std::unique_ptr<LoraTransferConfig> m_Config {};
    std::weak_ptr<class LoraTransferManager> m_Manager;
};

// One instance per physical controller
class LoraTransferManager final : public ITransferManager,
                                  public std::enable_shared_from_this<LoraTransferManager>
{
public:
    /**
     * initialize manager
     */
    bool Init() override { return false; }
    bool Deinit() override { return true; }

    /**
     * @return transport type
     */
    TransportType Transport() const override { return TransportType::Lora; }
    /**
     * Create and initialize a session from the given config
     */
    std::shared_ptr<ITransferSession> CreateSession(std::unique_ptr<ITransferConfig> cfg) override;

    // Enumerate active sessions
    std::vector<std::shared_ptr<ITransferSession>> ActiveSessions() const override;

public:
    /**
     * Class specific functions
     */
    bool Send(std::span<const std::byte> Data)
    {
        // the HAL is currently not thread safe, so all send must be synchronized through manager
        return m_OutgoingMessageQueue.Queue(std::vector {Data.begin(), Data.end()});
    };

    // Call this as often as possible
    void Process();

public:
    LoraTransferManager(std::unique_ptr<class TelemetryRadio> Radio) : m_Radio(std::move(Radio)) {}

protected:
    void ProcessIncomingMessage();

private:
    std::vector<std::shared_ptr<LoraTransferSession>> m_Sessions {};
    // messages are directly forwarded
    // ra::bricks::CircularBuffer<std::vector<std::byte>> m_IncomingMessageQueue {};
    ra::bricks::CircularBuffer<std::vector<std::byte>> m_OutgoingMessageQueue {};

    std::unique_ptr<class TelemetryRadio> m_Radio;
};
} // namespace ra::turtleford
