/**
 * @file Lock.h
 * @brief A mutex when the work queue runs jobs on its own thread, nothing when everything shares
 * one context.
 *
 * The mutex exists only when WORK_QUEUE_PREEMPTIVE is defined as non zero. Otherwise BasicLock::Guard
 * does nothing and costs nothing, so code written with it needs no change between the two modes.
 *
 * @code
 *     Lock m_Lock;
 *     ...
 *     const Lock::Guard Held {m_Lock};
 * @endcode
 */

#pragma once

#if defined(WORK_QUEUE_PREEMPTIVE) && WORK_QUEUE_PREEMPTIVE
    #include <mutex>
    #include <type_traits>
#endif

namespace ra
{
/**
 * @brief Serialises the callers that can run at the same time. Recursive lets the holder take it
 * again, for code whose callbacks call back in.
 *
 * Take it by constructing a Guard for the scope to protect. The lock itself has no other interface.
 *
 * @tparam Recursive  true for a std::recursive_mutex, false for a std::mutex. Without
 *                    WORK_QUEUE_PREEMPTIVE it makes no difference.
 */
template <bool Recursive>
class BasicLock
{
    // The work queue's mode decides whether more than one context can be calling at a time
#if defined(WORK_QUEUE_PREEMPTIVE) && WORK_QUEUE_PREEMPTIVE
    /** @brief The mutex type behind the lock. */
    using Mutex = std::conditional_t<Recursive, std::recursive_mutex, std::mutex>;

public:
    /** @brief Holds the lock from construction to destruction. */
    class Guard
    {
    public:
        /** @param Owner The lock to hold. Blocks until it is free (or, if Recursive, held by this thread). */
        explicit Guard(const BasicLock& Owner) : m_Held(Owner.m_Mutex) {}

    private:
        /** @brief The standard guard that does the locking. */
        std::lock_guard<Mutex> m_Held;
    };

private:
    /** @brief The mutex Guard locks. Mutable so a const lock can be held. */
    mutable Mutex m_Mutex;
#else
public:
    /** @brief Holds nothing: every caller shares one context. */
    class Guard
    {
    public:
        /** @param Owner Not used. */
        explicit Guard(const BasicLock&) {}
        /** @brief Does nothing. */
        ~Guard() {}
    };
#endif
};

/** @brief A lock that the holder must not take again. */
using Lock          = BasicLock<false>;
/** @brief A lock that the holder can take again, for code whose callbacks call back in. */
using RecursiveLock = BasicLock<true>;
} // namespace ra
