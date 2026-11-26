/**
 * @file Log.h
 * @brief ra::Logger: a process wide log that encodes entries to protobuf and caches them before
 * they go to storage.
 */

#pragma once

#include <array>
#include <cstdint>
// currently no support for format
// #include <format>
#include <span>
#include <string>
#include <string_view>

#include "Avionics_HAL.h"
#include "Config.h"
#include "DataStructure/Buffer/CachedBuffer.h"
#include "Lock.h"
#include "ProtoCodec.h"
#include "Type.h"

namespace ra
{
/**
 * @brief The log. One instance for the whole program, reached through Instance().
 *
 * Each Log() call encodes one entry as a framed Proto_LogMessage: the message, the severity, the
 * category and a timestamp. The entry goes into a cache of config::logger::CacheSize bytes. When the
 * cache is full, or Flush() is called, the cached entries are handed to the storage writer given to
 * RegisterCallback(). An entry that encodes to more than config::logger::BufferSize bytes is not
 * logged.
 *
 * Storing is serialised with a Lock, which is a mutex only when the work queue runs jobs on its own
 * thread.
 */
class Logger
{
    /** @brief Bytes of one entry's encode buffer. */
    constexpr static auto BufferSize = turtleford::config::logger::BufferSize;
    /** @brief Bytes cached before the storage writer is called. */
    constexpr static auto CacheSize  = turtleford::config::logger::CacheSize;
    /** @brief The cache in front of the storage writer. */
    using BufferType                 = bricks::CachedBuffer<CacheSize>;
    /** @brief The integer type of Severity. */
    using EnumType                   = uint_fast16_t;

public:
    /** @brief How serious an entry is. Stored in the entry as its numeric value. */
    enum class Severity : EnumType
    {
        Verbose = 0, ///< The most detailed level.
        Info,        ///< Normal events.
        Warn,        ///< Something unexpected.
        Error        ///< A failure.
    };

    /** @brief What every entry carries besides its message. */
    struct LogInfo
    {
        uint32_t Timestamp;      ///< Written to the entry as its timestamp, replacing the message's own.
        Severity Level;          ///< How serious it is.
        type::Category Category; ///< The part of the system it comes from.
    };

    /**
     * @brief The storage writer, called with the cached entries and the Ctx given to RegisterCallback().
     * Called as `Cb(std::span<const std::byte> Data, void* Ctx)`. It returns the number of bytes it
     * took, or 0 when it could not write. Bytes it did not take stay cached.
     */
    using StorageWriter = BufferType::StoreCallback;

public:
    /**
     * @brief The one logger.
     * @return The logger, created on first use.
     */
    static Logger& Instance()
    {
        static Logger Logger;
        return Logger;
    }

    // TODO : Separate package logic (to proto) from storing logic (writing to storage)
    /**
     * @brief Logs flight data.
     * @param Info  Timestamp, severity and category of the entry.
     * @param Data  The sensor readings.
     * @return Whether the entry was stored.
     */
    bool Log(const LogInfo& Info, const ra::type::FlightData& Data)
    { return Log(Info, turtleford::PbGen_FlightData(Data)); }

    /**
     * @brief Logs a status.
     * @param Info  Timestamp, severity and category of the entry.
     * @param Data  The status.
     * @return Whether the entry was stored.
     */
    bool Log(const LogInfo& Info, const ra::type::StatusMsg& Data) { return Log(Info, turtleford::PbGen_Status(Data)); }

    /**
     * @brief Logs a command.
     * @param Info  Timestamp, severity and category of the entry.
     * @param Data  The command.
     * @return Whether the entry was stored.
     */
    bool Log(const LogInfo& Info, const ra::type::CommandMsg& Data)
    { return Log(Info, turtleford::PbGen_CommandMsg(Data.Command)); }

    /**
     * @brief Logs text with a status code.
     *
     * Text longer than one message holds is split over several entries that share the same
     * timestamp, severity, category and status, written in order; joining them restores it.
     * Each entry's chunks_left says how many more follow. Entries logged from other threads at
     * the same time may land between them, so join by those fields,
     * not by position.
     *
     * @warning This is not thread safe: each entry is stored under the lock, but the entries of one
     * text are not stored as one, so entries from other threads can land between them.
     *
     * @param Info    Timestamp, severity and category of the entries.
     * @param Status  A status code that goes with the text.
     * @param Msg     The text.
     * @return Whether every entry was stored. Stops at the first one that was not.
     */
    bool Log(const LogInfo& Info, uint32_t Status, std::string_view Msg)
    {
        const size_t Count = turtleford::DebugChunkCount(Msg.size());
        for (size_t Index = 0; Index < Count; ++Index)
        {
            if (!Log(Info, turtleford::PbGen_DebugChunk(Status, Msg, Index))) { return false; }
        }

        return true;
    }

    /**
     * @brief Logs any MainMessage, stored as it is apart from the timestamp, which becomes Info.Timestamp.
     * @param Info     Timestamp, severity and category of the entry.
     * @param Message  The message.
     * @return Whether the entry was stored. false when it does not encode into BufferSize bytes
     *         or the cache has no room and the storage writer could not make any.
     */
    bool Log(const LogInfo& Info, const Proto_MainMessage& Message)
    {
        static thread_local std::array<std::byte, BufferSize> StaticBuffer;

        const auto Written = turtleford::ProtoEncode(Info.Timestamp,
                                                     static_cast<uint32_t>(Info.Level),
                                                     Info.Category,
                                                     Message,
                                                     StaticBuffer,
                                                     turtleford::ProtoFlags::Framed);

        if (Written == 0) { return false; }

        return Log(std::span<const std::byte>(StaticBuffer.data(), Written));
    }

    /**
     * @brief Stores bytes that are already serialized, as they are.
     * @note Serialized data ONLY
     * @param Data  A whole encoded entry, framed.
     * @return Whether it was stored. false for empty Data.
     */
    bool Log(std::span<const std::byte> Data)
    {
        if (Data.size_bytes() == 0) { return false; }

        const Lock::Guard Held {m_Lock};
        return m_Buffer.Store(Data);
    }

    /**
     * @brief Hand everything cached so far to the storage writer. Entries are otherwise only
     * written when the cache fills, which a quiet flight may never do: call this at the moments
     * that matter (a flight state change, landing, before power off).
     */
    void Flush()
    {
        const Lock::Guard Held {m_Lock};
        m_Buffer.Flush();
    }

    /**
     * @brief Sets where cached entries are written.
     * @param Cb   The storage writer, called as `Cb(std::span<const std::byte> Data, void* Ctx)`.
     *             It returns the number of bytes it took.
     * @param Ctx  Passed to Cb on every call. Not owned.
     */
    void RegisterCallback(StorageWriter Cb, void* Ctx)
    {
        const Lock::Guard Held {m_Lock};
        m_Buffer.RegisterCallback(Cb, Ctx);
    }

private:
    /** @brief Use Instance(). */
    Logger() {}
    /** @brief Not copyable. */
    Logger(Logger&)             = delete;
    /** @brief Not movable. */
    Logger(Logger&&)            = delete;
    /** @brief Not copyable. */
    Logger operator=(Logger&)   = delete;
    /** @brief Not movable. */
    Logger& operator=(Logger&&) = delete;

private:
    /** @brief Serialises Log(), Flush() and RegisterCallback(). */
    Lock m_Lock;
    /** @brief The cache in front of the storage writer. */
    BufferType m_Buffer;
};
} // namespace ra
