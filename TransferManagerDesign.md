# Centralized Transfer Architecture — Session-Based Design -- SEND

Thanks to Chatgpt for writing my docs once again

## 1. Overview

The goal of this design is to support multiple communication transports (e.g., BLE, TCP, RF, etc.) under a single central management system.
Each transport type may have different connection and configuration requirements (e.g., BLE GATT characteristics, TCP IP/port).
To avoid hard-coding transport-specific logic in the central controller, we introduce a **session-based abstraction**.

In this design, the system is composed of:

* **CentralTransferManager** — a registry and factory for different transport managers.
* **ITransferManager** — an interface implemented by each transport type (BLE, TCP, etc.) that knows how to create new connection sessions.
* **ITransferSession** — represents a single, active connection with send/receive capability.
* **ITransferConfig** — a polymorphic configuration base for per-transport connection parameters.

This separation allows the system to manage multiple concurrent connections, mix different transport types, and extend easily with new ones.

---

## 2. Architecture Overview

```
                +---------------------------+
                |  CentralTransferManager    |
                +---------------------------+
                  | registerManager()
                  | createSession(type, cfg)
                  v
        +---------------------+
        |   ITransferManager  |  <-- implemented by BLEManager, TCPManager, etc.
        +---------------------+
             | createSession(cfg)
             v
        +--------------------+
        |  ITransferSession  |  <-- represents one connection
        +--------------------+
          | send(), receive()
          | close()
```

Each `TransferManager` handles the knowledge of its own transport type
and can spawn multiple `TransferSession` instances, each with unique configuration.

---

## 3. Core Components

### 3.1 `ITransferConfig`

A lightweight, empty polymorphic base class used to represent configuration data.
Each transport defines its own derived configuration type.

Example:

```cpp
struct BLEConfig : public ITransferConfig {
    std::string deviceAddress;
    std::string gattWriteUUID;
    std::string gattReadUUID;
};
struct TCPConfig : public ITransferConfig {
    std::string ip;
    uint16_t port;
};
```

---

### 3.2 `ITransferSession`

Represents a live connection.
It exposes `send()`, `receive()`, and `close()` operations.
Each session instance owns its connection state and uses configuration provided during creation.

Example methods:

```cpp
bool send(const std::vector<uint8_t>& data);
std::vector<uint8_t> receive();
void close();
```

A session is usually short-lived and can be safely owned via `std::shared_ptr`.

---

### 3.3 `ITransferManager`

Responsible for creating sessions of a particular transport type.
Each concrete manager implements how to interpret its configuration and establish a connection.

Interface concept:

```cpp
class ITransferManager {
public:
    virtual std::string getType() const = 0;
    virtual std::shared_ptr<ITransferSession> createSession(
        const ITransferConfig& cfg) = 0;
};
```

Each subclass (e.g., `BLEManager`, `TCPManager`) knows how to construct its corresponding session type.

---

### 3.4 `CentralTransferManager`

The central coordinator that holds registered managers and creates sessions on request.
It knows *what* managers are available, but not *how* they connect.

High-level behavior:

1. Managers are registered once at startup.
2. A session is created by specifying a type (e.g., "BLE") and configuration.
3. The caller receives a session object used for communication.

Conceptual usage:

```cpp
CentralTransferManager central;
central.registerManager(std::make_shared<BLEManager>());
central.registerManager(std::make_shared<TCPManager>());

BLEConfig bleCfg{...};
TCPConfig tcpCfg{...};

auto bleSession = central.createSession("BLE", bleCfg);
auto tcpSession = central.createSession("TCP", tcpCfg);

bleSession->send(data1);
tcpSession->send(data2);
```

---

## 4. Data Flow Example

1. **Registration phase** — the central manager is initialized with available transport managers.

   ```cpp
   central.registerManager(std::make_shared<BLEManager>());
   central.registerManager(std::make_shared<TCPManager>());
   ```

2. **Session creation** — when a connection is needed, the caller supplies a config:

   ```cpp
   BLEConfig cfg{ "AA:BB:CC:DD:EE:FF", "writeUUID", "readUUID" };
   auto session = central.createSession("BLE", cfg);
   ```

   The central manager locates the correct manager by type ("BLE") and forwards the configuration.
   The BLE manager interprets the config, opens the connection, and returns a `BLESession` object.

3. **Data transfer** — all further operations (send, receive, close) are handled by the session itself.

4. **Closure** — when finished, the session cleans up transport resources.

---

## 5. Advantages

| Benefit                     | Description                                                                                         |
| --------------------------- | --------------------------------------------------------------------------------------------------- |
| **Extensibility**           | Adding a new transport (e.g., CAN, UART) requires only implementing a new Manager and Session pair. |
| **Encapsulation**           | Transport-specific logic is isolated inside its own manager and session.                            |
| **Scalability**             | Multiple concurrent connections (sessions) per transport are supported.                             |
| **Simplicity at top-level** | The central manager remains generic and type-agnostic.                                              |
| **Reusability**             | Sessions can be reused or pooled depending on connection lifetimes.                                 |

---

## 6. Lifecycle Summary

1. **Startup:**
   Register all available `ITransferManager` implementations.

2. **Configuration / Connection:**
   User or system provides a specific `ITransferConfig`.
   A manager interprets it and spawns a `ITransferSession`.

3. **Operation:**
   `ITransferSession` handles all communication independently.

4. **Shutdown:**
   Sessions are closed gracefully via `close()` or destroyed automatically.

---

## 7. Extending the System

To add a new transport:

1. Define a new configuration struct derived from `ITransferConfig`.
2. Implement a new `Session` class that inherits from `ITransferSession`.
3. Implement a new `Manager` that inherits from `ITransferManager`, returning instances of the new session.
4. Register the new manager in the central manager at startup.

No existing code needs modification — only extension.

---

## 8. Future Enhancements

* **Serialization Layer:** wrap sessions with a Protobuf message encoder/decoder to allow structured data transfer.
* **Async / Event Loop Integration:** extend sessions with asynchronous send/receive to support non-blocking I/O.
* **Connection Pooling:** central manager could manage active session pools for high-frequency reconnects.
* **Config Loading:** automatically generate configurations from JSON/YAML descriptors.

---

## 9. Summary

The session-based architecture cleanly separates connection setup from message transfer.
Each transport remains self-contained, the central manager stays generic, and the system can scale across multiple transport types and concurrent sessions.

This design strikes a balance between **extensibility**, **maintainability**, and **clarity**, while staying suitable for embedded, desktop, or gateway environments.


# Centralized Multi-Transport Transfer Architecture -- RECEIVE

This project provides a **flexible and extensible architecture** for managing multiple data transfer protocols (BLE, TCP, RF, etc.) with a **centralized manager** and **session-based transport handling**. It supports **polymorphic transport configurations**, **unified callbacks**, and **protobuf integration**.

---

## 1. Architecture Overview

### Goals

* Support **multiple transports** through a common interface.
* Centralize session management via **CentralTransferManager**.
* Allow **polymorphic transport configurations** (`ITransferConfig`) for context-aware callbacks.
* Enable **single unified callbacks** for send and receive.
* Keep sessions **isolated** from each other and from the central manager.

---

### Overall Architecture Flow

```
+------------------------+
|   CentralTransferMgr   |
|  (manages sessions)    |
+------------------------+
           │
           │ attachReceiveHandler(cb)
           ▼
+----------------+     +----------------+     +----------------+
| BLESession     |     | TCPSession     |     | RFSession      |
| (ITransferSess)|     | (ITransferSess)|     | (ITransferSess)|
+----------------+     +----------------+     +----------------+
       │                      │                     │
       │ notifyData()         │ notifyData()        │ notifyData()
       ▼                      ▼                     ▼
+----------------+     +----------------+     +----------------+
| TransferContext|     | TransferContext|     | TransferContext|
| - sessionID    |     | - sessionID    |     | - sessionID    |
| - transportType|     | - transportType|     | - transportType|
| - sourceID     |     | - sourceID     |     | - sourceID     |
| - config       |     | - config       |     | - config       |
+----------------+     +----------------+     +----------------+
       │                      │                     │
       └─────────► Unified Receive Callback ◄───────┘
                           │
                           ▼
                 Application / Protobuf Layer
```

**Explanation:**

* Each transport session is independent.
* `TransferContext` encapsulates **session metadata** and **polymorphic config**.
* Central manager attaches **one callback** for all sessions.
* Callback can downcast `config` to access **transport-specific info**.

---

## 2. Session Structure

```
       ITransferSession
       +--------------------+
       | send(data)          |
       | onReceive(cb)       |
       | getConfig()         |
       +--------------------+
                ▲
                │
    ----------------------------
    |            |            |
BLESession    TCPSession     RFSession
    |            |            |
    ▼            ▼            ▼
BLEConfig      TCPConfig     RFConfig
- deviceAddr   - ip/port     - freq/channel
- characteristics             - etc.
- mtu
```

* All sessions implement a common interface (`ITransferSession`).
* Each session stores **its own transport-specific config**, derived from `ITransferConfig`.
* Central manager and handlers see **only the base interface**, promoting extensibility.

---

## 3. Transfer Context

```cpp
struct ITransferConfig {
    virtual ~ITransferConfig() = default;
};
struct TransferContext {
    std::string sessionID;                      // Unique session ID
    std::string transportType;                  // e.g., "BLE", "TCP", "RF"
    std::string sourceID;                       // e.g., characteristic UUID, port
    std::shared_ptr<ITransferConfig> config;   // Polymorphic transport config
};
```

* Provides **full metadata** for received data.
* `config` is **polymorphic**, allowing callbacks to access transport-specific fields.
* TransferConfig and TransferContext should be separate since they represents different things

---

## 4. Receive Callback

```cpp
using ReceiveCallback = std::function<void(
    TransferContext context,
    const std::vector<uint8_t>& data
)>;
```

* Called by **any session** when data arrives.
* Receives `TransferContext` for transport/session-specific metadata.
* Safe downcasting enables **access to transport-specific fields**.

---

### Example: BLE Session Notify

```cpp
void BLESession::notifyData(const std::vector<uint8_t>& data, const std::string& charUUID) {
    if (recvCb_) {
        TransferContext ctx{
            getID(),
            getTransportType(),
            charUUID,
            bleConfig_ // shared_ptr<BLEConfig> stored as ITransferConfig
        };
        recvCb_(ctx, data);
    }
}
```

### Example: TCP Session Notify

```cpp
void TCPSession::notifyData(const std::vector<uint8_t>& data) {
    if (recvCb_) {
        TransferContext ctx{
            getID(),
            getTransportType(),
            std::to_string(tcpConfig_->port),
            tcpConfig_ // shared_ptr<TCPConfig> stored as ITransferConfig
        };
        recvCb_(ctx, data);
    }
}
```

---

## 5. Unified Handler Example

```cpp
auto handler = [](TransferContext ctx, const std::vector<uint8_t>& data) {
    std::cout << "[" << ctx.transportType << "][" << ctx.sessionID
              << "][" << ctx.sourceID << "] Received "
              << data.size() << " bytes\n";

    if (ctx.transportType == "BLE") {
        auto bleCfg = std::dynamic_pointer_cast<BLEConfig>(ctx.config);
        std::cout << "BLE Device: " << bleCfg->deviceAddress << "\n";
    } else if (ctx.transportType == "TCP") {
        auto tcpCfg = std::dynamic_pointer_cast<TCPConfig>(ctx.config);
        std::cout << "TCP IP: " << tcpCfg->ip << ":" << tcpCfg->port << "\n";
    }
};
```

* The **same handler** can be attached to all sessions.
* Downcasting gives access to **transport-specific metadata**.
* Supports multiple sessions without duplicating logic.

---

## 6. CentralTransferManager Integration

```cpp
CentralTransferManager central;

central.attachReceiveHandler(handler);

auto bleSession = central.createSession("BLE", bleConfig);
auto tcpSession = central.createSession("TCP", tcpConfig);
```

* Central manager manages **all sessions**.
* Sessions notify the manager via callbacks.
* The handler sees **all incoming data** with its context.

---

## 7. Protobuf Layer Integration

```
 BLE / TCP / RF Session
           │
           ▼
   +--------------------+
   | TransferContext     |
   | + ITransferConfig   |
   +--------------------+
           │
           ▼
 Unified Receive Callback
           │
           ▼
   +--------------------+
   | Protobuf Deserialize|
   | / Application Logic |
   +--------------------+
```

* Raw bytes are decoded in the **application layer**.
* `config` provides transport-specific metadata.
* Supports **multi-transport handling** with a single handler.

---

## 8. Advantages

| Benefit               | Description                                                    |
| --------------------- | -------------------------------------------------------------- |
| **Unified handler**   | Single callback receives data from all transports.             |
| **Context-rich**      | Session ID, transport type, source ID, and polymorphic config. |
| **Safe polymorphism** | Downcast to access transport-specific fields.                  |
| **Extensible**        | Add new transport types without modifying central manager.     |
| **Session isolation** | Transport logic stays inside session.                          |
| **Metadata storage**  | Callbacks can store extra info like timestamps, RSSI, etc.     |

