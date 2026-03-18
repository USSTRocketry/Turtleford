/**
 * @file TransferSession.h
 * @brief Transport interfaces for multi-protocol data transfer.
 */

#pragma once

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ra::turtleford
{
/**
 * @brief Supported transport types.
 */
enum class TransportType
{
    None,
    Lora,
    BLE
};
/**
 * @brief Base interface for transport configuration.
 *
 * Each transport (LoRa, BLE, etc.) derives from this to provide
 * its own configuration parameters. Use Clone() to create copies
 * for multiple sessions.
 */
struct ITransferConfig
{
    virtual std::unique_ptr<ITransferConfig> Clone() const = 0;
    virtual ~ITransferConfig()                             = default;
};

/**
 * @brief Metadata passed to receive callbacks.
 *
 * Contains session ID, transport type, source ID, and session reference.
 * Use this to identify where data came from and respond appropriately.
 */
struct TransferContext
{
    size_t SessionID;
    TransportType Type;
    size_t SourceID;
    std::shared_ptr<class ITransferSession> Session;
};

/**
 * @brief Interface for a transport communication session.
 *
 * Represents a connection over LoRa, BLE, etc. Use Send() to transmit
 * data and RegisterCallback() to receive data. The manager creates
 * and owns sessions.
 */
class ITransferSession
{
public:
    using ReceiveCallback = std::function<void(const TransferContext& Context, std::span<const std::byte> Data)>;

    virtual bool Send(std::span<const std::byte> Data) = 0;
    virtual void RegisterCallback(ReceiveCallback Cb)  = 0;

    virtual bool IsOpen()                         = 0;
    virtual size_t ID() const                     = 0;
    virtual TransportType Transport() const       = 0;
    virtual const ITransferConfig& Config() const = 0;

    virtual ~ITransferSession() = default;

private:
    friend std::shared_ptr<ITransferSession> std::make_shared<ITransferSession>();
};

/**
 * @brief Interface for managing transport sessions.
 *
 * Each transport type (LoRa, BLE, etc.) implements this to create
 * and manage sessions. Use CreateSession() to get a session handle
 * and Close() to terminate it.
 */
struct ITransferManager
{
    virtual ~ITransferManager() = default;

    virtual bool Init()   = 0;
    virtual bool Deinit() = 0;

    virtual TransportType Transport() const = 0;

    virtual std::shared_ptr<ITransferSession> CreateSession(std::unique_ptr<ITransferConfig> cfg) = 0;
    virtual bool Close(const ITransferSession&)                                                   = 0;
    virtual std::shared_ptr<ITransferSession> GetSession(size_t id) const                         = 0;
    virtual void Process()                                                                        = 0;
};

/**
 * @brief Interface for discovering available transport endpoints.
 *
 * Use Discover() to find available devices and ConfigFromEndpoint()
 * to convert a discovered endpoint into a configuration for creating
 * sessions.
 */
struct IDiscoverable
{
    struct Endpoint
    {
        std::string Id;   // MAC, UUID, IP, COM port
        std::string Name; // Friendly name
        std::any Meta;    // RSSI, advert data, etc.
    };

    virtual ~IDiscoverable()                                                     = default;
    virtual std::vector<Endpoint> Discover()                                     = 0;
    virtual std::unique_ptr<ITransferConfig> ConfigFromEndpoint(const Endpoint&) = 0;
};

/**
 * @brief Asynchronous version of IDiscoverable.
 *
 * Use StartDiscovery() to begin continuous scanning with callbacks
 * for each discovered endpoint, and StopDiscovery() when done.
 */
struct IAsyncDiscoverable : public IDiscoverable
{
    using Callback                        = std::function<void(const Endpoint&)>;
    virtual ~IAsyncDiscoverable()         = default;
    virtual void StartDiscovery(Callback) = 0;
    virtual void StopDiscovery()          = 0;
};

/**
 * @brief Typed endpoint identifier for security operations.
 *
 * Combines transport type with endpoint ID for secure pairing
 * and authentication operations.
 */
struct EndpointIdentifier
{
    TransportType Type;
    std::string Id; // MAC, UUID, IP, COM port
    std::any Meta;  // optional extra info
};

/**
 * @brief Interface for secure device pairing and authentication.
 *
 * Use Pair() to establish secure connections with endpoints
 * after discovery for encrypted communication.
 */
struct ISecurityAssociation
{
    virtual ~ISecurityAssociation()              = default;
    virtual bool Pair(const EndpointIdentifier&) = 0;
};

/**
 * @brief Central router for managing multiple transport types.
 *
 * Registers different transport managers and provides unified
 * interface for sending/receiving across all transports with
 * automatic fallback and routing.
 */
struct ITransportRouter
{
    using ReceiveCallback = std::function<void(TransportType, std::span<const std::byte>)>;

    virtual ~ITransportRouter() = default;

    virtual void RegisterManager(std::unique_ptr<ITransferManager> mgr) = 0;
    virtual ITransferManager* GetManager(TransportType type)            = 0;

    virtual void RegisterCallback(ReceiveCallback Cb) = 0;

    virtual bool Send(std::span<const std::byte> Data, TransportType Preferred = TransportType::None) = 0;

    virtual std::shared_ptr<ITransferSession> CreateSession(TransportType Type,
                                                            std::unique_ptr<ITransferConfig> Cfg)       = 0;
    virtual std::shared_ptr<ITransferSession> CreateSession(TransportType Type,
                                                            std::shared_ptr<const ITransferConfig> Cfg) = 0;

    /**
     * @brief Get a session for a transport by ID.
     */
    virtual std::shared_ptr<ITransferSession> GetSession(TransportType Type, size_t id) const = 0;
};
} // namespace ra::turtleford
