// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Screen-translation orchestrator. Owns the worker thread, the translation
// cache and the latest results. It is intentionally decoupled from the
// renderer / emuenv: callers feed it RGBA frames and poll for results.
// See dev-docs/screen-translation/02-architecture.md

#pragma once

#include <translator/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace translator {

struct TranslatorState {
    TranslatorState();
    ~TranslatorState();

    TranslatorState(const TranslatorState &) = delete;
    TranslatorState &operator=(const TranslatorState &) = delete;

    // Update runtime options (called when config changes).
    void set_options(const Options &opts);
    Options get_options() const;

    // Toggle the overlay visibility. Turning it ON also requests one
    // translation run of the next submitted frame.
    void toggle();
    void set_visible(bool visible);
    bool is_visible() const;

    // Trigger mode (0 = hotkey, 1 = auto). Auto mode periodically re-runs.
    void set_mode(int mode, uint64_t interval_us);

    // Should the caller capture and submit a new frame right now?
    bool wants_new_frame(uint64_t now_us) const;

    // Submit a captured RGBA8 frame. Pixels are copied internally; the worker
    // coalesces to the most recent submission.
    void submit_frame(const uint32_t *rgba, uint32_t width, uint32_t height,
        uint64_t now_us);

    // Poll for the latest results. Returns true (and fills `out`) only when a
    // newer generation than last consumed is available.
    bool take_results(std::vector<TranslatedLine> &out);

    // Short user-facing status (empty when nothing to report).
    std::string status_message() const;

private:
    void worker_main();
    void run_pipeline(std::vector<uint32_t> pixels, uint32_t width, uint32_t height);
    void set_status(const std::string &msg);

    std::thread m_worker;
    mutable std::mutex m_mtx;
    std::condition_variable m_cv;

    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_visible{ false };
    bool m_request_run = false; // guarded by m_mtx

    struct Job {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::optional<Job> m_pending; // guarded by m_mtx (coalesced)

    Options m_opts; // guarded by m_mtx

    std::vector<TranslatedLine> m_results; // guarded by m_mtx
    uint64_t m_generation = 0; // guarded by m_mtx
    uint64_t m_consumed_generation = 0; // only touched by consumer

    std::unordered_map<std::string, std::string> m_cache; // worker-only

    int m_mode = 0; // guarded by m_mtx
    uint64_t m_interval_us = 3'000'000; // guarded by m_mtx
    std::atomic<uint64_t> m_last_run_us{ 0 };

    std::string m_status; // guarded by m_mtx
};

} // namespace translator
