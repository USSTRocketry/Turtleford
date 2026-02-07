#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>
#include <array>
#include <algorithm>
#include <limits>

#include "TransferSession.h"
#include "CircularBuffer.h"
#include "Avionics_HAL.h"

namespace ra::turtleford
{
/**
 * @brief Configuration for LoRa transfer sessions.
 *
 * Contains the peer ID for addressing and a user-defined context pointer.
 * The context can be any user data and is not managed by the transport.
 */
struct LoraTransferConfig final : public ITransferConfig
{
    struct
    {
        uint8_t SelfId;
        /** @brief Peer identifier for routing messages to specific devices. */
        uint8_t RecvFrom;
        /** @brief Local identifier for this device in the session. */
        uint8_t SendTo;
    } Addr {};

    /** @brief User-defined context pointer. Not managed by the transport. */
    void* Ctx = nullptr;

public:
    /**
     * @brief Creates a copy of this configuration.
     * @return Unique pointer to the cloned configuration.
     */
    std::unique_ptr<ITransferConfig> Clone() const override { return std::make_unique<LoraTransferConfig>(*this); }

public:
    LoraTransferConfig(uint8_t Peer, uint8_t Local, void* Context) :
        Addr {.SelfId = Local, .RecvFrom = Peer, .SendTo = Peer}, Ctx(Context)
    {
    }
    LoraTransferConfig(const LoraTransferConfig&)            = default;
    LoraTransferConfig& operator=(const LoraTransferConfig&) = default;
};

/**
 * @brief Manages LoRa communication sessions and radio operations.
 *
 * There should be one manager per physical LoRa radio controller.
 * The manager handles session creation, message queuing, and radio operations.
 * Call Process() frequently to handle send/receive operations.
 */
class LoraTransferManager final : public ITransferManager,
                                  public std::enable_shared_from_this<LoraTransferManager>
{
private:
    /**
     * @brief Represents a thin public-facing LoRa communication session.
     *
     * The session is a lightweight wrapper (id + weak manager). Per-session state
     * (config, callback) is stored in ManagedSession to improve locality and
     * reduce per-session footprint.
     */
    class LoraTransferSession final : public ITransferSession
    {
    public:
        bool Send(std::span<const std::byte> Data) override;
        void RegisterCallback(ITransferSession::ReceiveCallback Cb) override;
        bool IsOpen() override;
        size_t ID() const override;
        TransportType Transport() const override { return TransportType::Lora; }
        const ITransferConfig& Config() const override;

    public:
        ~LoraTransferSession() = default;

    private:
        LoraTransferSession(std::weak_ptr<LoraTransferManager> Manager, size_t SessionId) :
            m_Manager(std::move(Manager)), m_SessionId(SessionId)
        {
        }

    private:
        size_t m_SessionId {};
        std::weak_ptr<LoraTransferManager> m_Manager;

        friend class LoraTransferManager;
    };

public:
    /**
     * @brief Initializes the LoRa manager.
     * @return True if initialization successful, false otherwise.
     */
    bool Init() override { return true; }

    /**
     * @brief Deinitializes the LoRa manager.
     * @return True if deinitialization successful, false otherwise.
     */
    bool Deinit() override { return true; }

    /**
     * @brief Gets the transport type.
     * @return returns TransportType for this manager.
     */
    TransportType Transport() const override { return TransportType::Lora; }

    /**
     * @brief Creates a new LoRa session with the given configuration.
     * @param cfg Unique pointer to the LoRa transfer configuration.
     * @return Shared pointer to the created session, or nullptr if configuration is invalid.
     *
     * The session is automatically added to the active session list
     * The manager takes ownership of the config.
     */
    std::shared_ptr<ITransferSession> CreateSession(std::unique_ptr<ITransferConfig> Cfg) override;

    /**
     * @brief Closes and removes a specific session from the manager.
     * @param Session Reference to the session to close.
     *
     * The session will be removed from the managed list.
     */
    bool Close(const ITransferSession& Session) override;

    /**
     * @brief Get a session by its ID.
     */
    std::shared_ptr<ITransferSession> GetSession(size_t Id) const override;

    /**
     * @brief Processes queued messages and handles radio operations.
     *
     * This method should be called as frequently as possible to:
     * - Send queued outgoing messages
     * - Receive incoming messages from the radio
     * - Distribute received messages to appropriate session callbacks
     *
     * Note: Currently assumes single-threaded operation for receive processing.
     */
    void Process();

public:
    /**
     * @brief Constructs a new LoRa transfer manager.
     * @param Radio Unique pointer to the _already initialized_ telemetry radio.
     *
     * The manager takes ownership of the radio interface.
     */
    LoraTransferManager(std::unique_ptr<ITelemetryRadio> Radio) : m_Radio(std::move(Radio)) {}

private:
    static constexpr auto LoraMTU = 512;
    static_assert(LoraMTU > 0, "LoraMTU must be a positive value");

    bool Send(size_t SessionId, std::span<const std::byte> Data);

    // Helper methods to set per-session state.
    void SetCallbackForSession(size_t Id, ITransferSession::ReceiveCallback Cb);
    bool IsSessionAlive(size_t Id) const;
    const ITransferConfig* GetConfig(size_t Id) const;

private:
    struct ManagedSession
    {
        std::shared_ptr<LoraTransferSession> Session;
        ITransferSession::ReceiveCallback Callback;
        std::unique_ptr<LoraTransferConfig> Config;
    };

    struct OutgoingMessage
    {
        uint8_t DestinationId;
        uint8_t SourceId;
        uint16_t Length;
        std::array<std::byte, LoraMTU> Data;
    };
    static_assert(LoraMTU <= std::numeric_limits<decltype(OutgoingMessage::Length)>::max(),
                  "LoraMTU exceeds capacity of OutgoingMessage::Length");

private:
    std::vector<ManagedSession> m_Sessions {}; // Active sessions
    ra::bricks::CircularBuffer<OutgoingMessage> m_OutgoingMessageQueue {};

    std::unique_ptr<ITelemetryRadio> m_Radio;
    // Next session id to assign (monotonic)
    size_t m_NextSessionId {1};
};
} // namespace ra::turtleford
