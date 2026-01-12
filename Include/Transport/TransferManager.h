#pragma once

#include <memory>

#include "TransferSession.h"

namespace ra::turtleford
{
/*
 * @brief Central manager that tracks all persistent sessions and optionally wires a unified receive callback.
 * @note Sessions remain autonomous and handle their own send/receive.
 */
class CentralTransferManager
{
public:
    /*
     * @brief Register a persistent session with the manager
     * @param Session Shared pointer to an autonomous session
     * @return Shared pointer to the session if success, nullptr otherwise
     */
    virtual std::shared_ptr<ITransferSession> RegisterSession(std::shared_ptr<ITransferSession> Session) = 0;

    /*
     * @brief Unregister a session by its session ID
     * @param SessionID The unique ID of the session
     */
    virtual void UnregisterSession(size_t SessionID) = 0;

    /*
     * @brief Attach a unified receive callback to all current and future sessions
     * @param Callback Callback function invoked when any session receives data
     */
    virtual void UniversalReceiveHandler(ITransferSession::ReceiveCallback Callback) = 0;

    /*
     * @brief Lookup a session by its ID
     * @param SessionID The unique session identifier
     * @return Shared pointer to the session if found, nullptr otherwise
     */
    virtual std::shared_ptr<ITransferSession> GetSessionByID(size_t SessionID) const = 0;

public:
    /// @brief Type of the unified receive callback
    /// @param Context Transfer metadata, includes SessionID, TransportType, SourceID, and Config pointer
    /// @param Data Raw received bytes

    virtual ~CentralTransferManager() = default;
};
} // namespace ra::turtleford
