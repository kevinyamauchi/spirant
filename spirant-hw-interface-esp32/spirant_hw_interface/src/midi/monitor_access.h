#pragma once

#include <stdint.h>

#include "monitor_state.h"

// --- Serialising the monitor snapshot ----------------------------------------
//
// MonitorState is written by the drain loop (loopTask, core 1, priority 1) and
// read by the UI task (core 1, priority 3). One writer, one reader, same core,
// and the reader preempts the writer.
//
// **Why this is a mutex and not a seqlock.** A seqlock is the textbook answer
// for one-writer/one-reader latest-value-wins, and it is wrong here. Its reader
// spins until the writer finishes its update. The reader is the *higher
// priority* task on the *same core*, so if it preempts the writer mid-update it
// spins forever on a writer that can never be scheduled to finish. That is a
// hard hang, not a slow path. Any lock-free scheme with a retrying reader has
// the same defect on this pairing.
//
// A FreeRTOS mutex is correct precisely because it does priority inheritance:
// the blocked UI task lends its priority to the drain, which then completes.
//
// Cost is one take/give per drain pass, not per message -- the drain applies a
// whole batch inside a single critical section. At the section 2.3 traffic
// estimate that is on the order of a thousand lock operations per second, which
// is noise next to a 1539 us value redraw.
//
// The reader takes a timeout: a wedged drain degrades the monitor page to stale
// data rather than freezing the UI task, the same posture as Wire.setTimeOut()
// in config.h. Nothing here touches ParameterValues, which stays single-writer.

namespace monitor
{

/// Creates the mutex. Call from setup() before the UI task can run. Safe to
/// call twice.
bool init();

/// Exclusive access for the drain. Returns nullptr if the mutex could not be
/// taken within `timeout_ms`, in which case do NOT call endWrite().
///
/// Prefer the WriteLock helper below over calling these directly.
MonitorState* beginWrite(uint32_t timeout_ms);
void          endWrite();

/// Copy the current snapshot out. Returns false if the mutex was unavailable
/// within `timeout_ms`, leaving `out` untouched -- render the previous copy.
bool snapshot(MonitorSnapshot& out, uint32_t timeout_ms);

/// Scoped write access.
///
///   { monitor::WriteLock w(5); if (w) w->onMessage(msg, ms, us); }
///
/// Check it before use: an unavailable mutex yields a null state, and dropping
/// a monitor update is always better than blocking the drain.
class WriteLock
{
  public:
    explicit WriteLock(uint32_t timeout_ms) : state_(beginWrite(timeout_ms)) {}
    ~WriteLock()
    {
        if (state_ != nullptr) endWrite();
    }

    WriteLock(const WriteLock&)            = delete;
    WriteLock& operator=(const WriteLock&) = delete;

    explicit operator bool() const { return state_ != nullptr; }
    MonitorState* operator->() const { return state_; }
    MonitorState& operator*() const { return *state_; }

  private:
    MonitorState* state_;
};

}  // namespace monitor
