#pragma once

#include <span>
#include <vector>

#include "TransferSession.h"
#include "CircularBuffer.h"
#include "Avionics_HAL.h"

namespace ra::turtleford
{
class LoraTransferManager;

/**
 * @brief Configuration for LoRa transfer sessions.
 *
 * Contains the peer ID for addressing and a user-defined context pointer.
 * The context can be any user data and is not managed by the transport.
 */
struct LoraTransferConfig final : public ITransferConfig
{
    /** @brief Peer identifier for routing messages to specific devices. */
    uint8_t PeerId = 0;

    /** @brief User-defined context pointer. Not managed by the transport. */
    void* Ctx = nullptr;

public:
    /**
     * @brief Creates a copy of this configuration.
     * @return Unique pointer to the cloned configuration.
     */
    std::unique_ptr<ITransferConfig> Clone() const override { return std::make_unique<LoraTransferConfig>(*this); }

public:
    LoraTransferConfig(uint8_t Id, void* Context) : PeerId(Id), Ctx(Context) {};
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
     * @brief Represents a LoRa communication session.
     *
     * A session provides a handle for sending and receiving data through LoRa.
     * Multiple sessions can be created for different peers. Sessions are
     * managed by a LoraTransferManager and should be created through it.
     */
    class LoraTransferSession final : public ITransferSession
    {
    public:
        bool Send(std::span<const std::byte> Data) override;
        void RegisterCallback(ReceiveCallback Cb) override { m_Callback = Cb; }
        bool IsOpen() override { return !m_Token.expired() && !m_Manager.expired(); };
        size_t ID() const override { return m_SessionId; }
        TransportType Transport() const override { return TransportType::Lora; }
        const ITransferConfig& Config() const override { return *m_TransferConfig; }

    private:
        struct SessionToken
        {
        };
        LoraTransferSession(std::weak_ptr<LoraTransferManager> Manager,
                            std::unique_ptr<LoraTransferConfig> Cfg,
                            std::shared_ptr<SessionToken> Token,
                            size_t SessionId) :
            m_TransferConfig(std::move(Cfg)),
            m_Manager(std::move(Manager)),
            m_Token(std::move(Token)),
            m_SessionId(SessionId)
        {
        }

        void Deliver(const TransferContext& Ctx, std::span<const std::byte> Payload)
        {
            if (m_Callback) m_Callback(Ctx, Payload);
        }

    private:
        size_t m_SessionId {};
        ITransferSession::ReceiveCallback m_Callback {};
        std::unique_ptr<LoraTransferConfig> m_TransferConfig {};
        std::weak_ptr<LoraTransferManager> m_Manager;
        std::weak_ptr<SessionToken> m_Token;

        // Only the manager should invoke Deliver to forward received payloads to the
        // session's callback. Keep this private to avoid exposing extra API surface.

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
    std::shared_ptr<ITransferSession> CreateSession(std::unique_ptr<ITransferConfig> cfg) override;

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
    std::shared_ptr<ITransferSession> GetSession(size_t id) const override;

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
    bool Send(std::span<const std::byte> Data)
    {
        // the HAL is currently not thread safe, so all send must be synchronized through manager
        return m_OutgoingMessageQueue.Queue(std::vector<std::byte> {Data.begin(), Data.end()});
    };

private:
    struct ManagedSession
    {
        std::shared_ptr<LoraTransferSession> Session;
        std::shared_ptr<LoraTransferSession::SessionToken> Token;
    };

    std::vector<ManagedSession> m_Sessions {};
    // messages are directly forwarded
    ra::bricks::CircularBuffer<std::vector<std::byte>> m_OutgoingMessageQueue {};

    std::unique_ptr<ITelemetryRadio> m_Radio;
    // Next session id to assign (monotonic)
    size_t m_NextSessionId {1};
};
} // namespace ra::turtleford
