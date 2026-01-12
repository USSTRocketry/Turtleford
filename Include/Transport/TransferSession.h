#pragma once

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ra::turtleford
{
enum class TransportType
{
    None,
    Lora,
    BLE
};
/**
 * Transport specific data
 */
struct ITransferConfig
{
    virtual std::unique_ptr<ITransferConfig> Clone() const = 0;
    virtual ~ITransferConfig()                             = default;
};
struct TransferContext
{
    size_t SessionID;
    TransportType Type;
    size_t SourceID;
    std::shared_ptr<class ITransferSession> Session;
};

struct ITransferSession
{
    using ReceiveCallback = std::function<void(const TransferContext& Context, std::span<const std::byte> Data)>;

    virtual bool Send(std::span<const std::byte> Data) = 0;
    virtual void RegisterCallback(ReceiveCallback Cb)  = 0;
    virtual void Close()                               = 0;

    virtual bool IsOpen()                         = 0;
    virtual size_t ID() const                     = 0;
    virtual TransportType Transport() const       = 0;
    virtual const ITransferConfig& Config() const = 0;

    virtual ~ITransferSession() = default;

private:
    friend std::shared_ptr<ITransferSession> std::make_shared<ITransferSession>();
};

struct ITransferManager
{
    virtual ~ITransferManager() = default;

    virtual bool Init()   = 0;
    virtual bool Deinit() = 0;

    virtual TransportType Transport() const = 0;

    // Create session: takes ownership of a polymorphic config
    virtual std::shared_ptr<ITransferSession> CreateSession(std::unique_ptr<ITransferConfig> cfg) = 0;
    // Enumerate active sessions
    virtual std::vector<std::shared_ptr<ITransferSession>> ActiveSessions() const                 = 0;
};

// =====================
// Optional Capability: Discoverable
// =====================
struct IDiscoverable
{
    struct Endpoint
    {
        std::string Id;   // MAC, UUID, IP, COM port, string is more flexible than other types here
        std::string Name; // Friendly name
        std::any Meta;    // RSSI, advert data, etc.
    };

    virtual ~IDiscoverable()                 = default;
    virtual std::vector<Endpoint> Discover() = 0;

    // Convert a discovered endpoint into a valid config
    virtual std::unique_ptr<ITransferConfig> ConfigFromEndpoint(const Endpoint& ep) = 0;
};

// Optional async discovery
struct IAsyncDiscoverable : public IDiscoverable
{
    using Callback                           = std::function<void(const Endpoint&)>;
    virtual ~IAsyncDiscoverable()            = default;
    virtual void StartDiscovery(Callback cb) = 0;
    virtual void StopDiscovery()             = 0;
};

struct EndpointIdentifier
{
    TransportType Type;
    std::string Id; // MAC, UUID, IP, COM port
    std::any Meta;  // optional extra info
};

// =====================
// Security / Pairing
// =====================
struct ISecurityAssociation
{
    virtual ~ISecurityAssociation() = default;

    // Use typed endpoint identifier instead of plain string
    virtual bool Pair(const EndpointIdentifier& endpoint) = 0;
};

// =====================
// Transport Router / Central Manager Interface
// =====================
struct ITransportRouter
{
    using ReceiveCallback = std::function<void(TransportType, std::span<const std::byte>)>;

    virtual ~ITransportRouter() = default;

    virtual void RegisterManager(std::unique_ptr<ITransferManager> mgr) = 0;
    virtual ITransferManager* GetManager(TransportType type)            = 0;

    virtual void RegisterCallback(ReceiveCallback Cb) = 0;

    // Send data over preferred transport, fallback if unavailable
    virtual bool Send(std::span<const std::byte> Data, TransportType Preferred = TransportType::None) = 0;

    virtual std::shared_ptr<ITransferSession> CreateSession(TransportType Type,
                                                            std::unique_ptr<ITransferConfig> Cfg)       = 0;
    // Create session using a specific transport
    virtual std::shared_ptr<ITransferSession> CreateSession(TransportType Type,
                                                            std::shared_ptr<const ITransferConfig> Cfg) = 0;

    // Enumerate all active sessions
    virtual std::vector<std::shared_ptr<ITransferSession>> ActiveSessions() const = 0;
};
} // namespace ra::turtleford
