#pragma once
#include <atomic>
#include <thread>
#include <functional>
#include <string>

// ============================================================================
// LoadingManager — runs heavy CPU world-gen work on a background thread while
// the main thread keeps rendering a loading screen + progress bar.
//
// IMPORTANT OpenGL constraint: GL calls must stay on the main thread. So the
// split is:
//   - background thread: pure-CPU work (terrain heightmap, decor placement,
//     army spawn into CPU arrays) — reported via atomic progress/stage.
//   - main thread: after the worker signals done, run the GL uploads
//     (heightmap -> GPU, SDF init, compute init) in finalize().
//
// Usage:
//   loader.start([&](LoadingManager& L){ ...cpu work...; L.set(0.5f,"..."); });
//   each frame: if (loader.cpu_done() && !loader.finalized) { ...GL uploads...; loader.finalized=true; }
// ============================================================================
class LoadingManager {
public:
    std::atomic<float> progress{0.0f};      // 0..1 (CPU phase)
    std::atomic<bool> running{false};
    std::atomic<bool> cpu_complete{false};
    bool finalized = false;                  // main-thread GL finalize done
    char stage_buf[64] = "";                 // current stage label (main reads)

    // Begin background CPU work. `work` runs on the worker thread.
    void start(std::function<void(LoadingManager&)> work) {
        if (running.load()) return;
        progress.store(0.0f);
        cpu_complete.store(false);
        finalized = false;
        running.store(true);
        set(0.0f, "STARTING");
        worker = std::thread([this, work]() {
            work(*this);
            cpu_complete.store(true);
            running.store(false);
        });
        worker.detach();
    }

    // Worker calls this to report progress + a stage label.
    void set(float p, const char* stage) {
        progress.store(p);
        // copy label (single writer = worker thread; main only reads)
        size_t i = 0;
        for (; stage[i] && i < sizeof(stage_buf) - 1; i++) stage_buf[i] = stage[i];
        stage_buf[i] = '\0';
    }

    bool cpu_done() const { return cpu_complete.load(); }
    float get_progress() const { return progress.load(); }
    std::string get_stage() const { return std::string(stage_buf); }

    // Reset to a fresh state so the manager can be reused for another load.
    // Only call when not running.
    void reset() {
        progress.store(0.0f);
        running.store(false);
        cpu_complete.store(false);
        finalized = false;
        stage_buf[0] = '\0';
    }

private:
    std::thread worker;
};
