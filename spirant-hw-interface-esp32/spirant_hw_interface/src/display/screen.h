#pragma once

class DisplayManager;

// --- One screen of the instrument --------------------------------------------
//
// The refactor plan section 2.4 asks for, done before the monitor page rather
// than after: DisplayManager::update() used to take ParameterValues& directly,
// which left no room for a screen that renders something else. Each screen now
// pulls the state it needs.
//
// Every method runs on the UI task. The panel has exactly one writer (plan
// section 1.8) and nothing here changes that.

class Screen
{
  public:
    virtual ~Screen() = default;

    /// Shown in the header. Never returns nullptr.
    virtual const char* title() const = 0;

    /// Right-hand header annotation -- page counter, connection state. Return
    /// nullptr for none. The buffer is the caller's; write into it.
    virtual const char* headerNote(char* buf, unsigned buf_len) const
    {
        (void)buf;
        (void)buf_len;
        return nullptr;
    }

    /// Refresh whatever the screen renders from, before anything is drawn or
    /// asked about. This is where a screen backed by shared state takes its
    /// snapshot, so that title(), headerNote(), needsFullRepaint() and the
    /// render calls all see one consistent version of it.
    virtual void poll() {}

    /// True when the screen's own state has changed in a way that incremental
    /// rendering cannot express -- a parameter page switch, for instance, where
    /// every region and the header note change at once. Checked on each update()
    /// before the incremental path, so a screen never has to wait a tick for
    /// dm.invalidate() to take effect.
    virtual bool needsFullRepaint() const { return false; }

    /// Repaint everything, including static furniture. Called when the screen
    /// is activated and whenever the manager is invalidated. This is the
    /// expensive path: budget it against section 1.3's full-page figure.
    virtual void renderFull(DisplayManager& dm) = 0;

    /// Repaint only what has changed since the last call. Runs on the UI task's
    /// 20 ms tick, so it must be cheap and it must be idempotent when nothing
    /// has changed -- returning immediately is the common case.
    virtual void renderIncremental(DisplayManager& dm) = 0;

    /// Called once when the screen becomes active, before the first
    /// renderFull(). Somewhere to drop cached "last drawn" values so the first
    /// incremental pass after a screen switch does not skip a stale region.
    virtual void onEnter() {}
};
