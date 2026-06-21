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

#include <translator/state.h>

#include <translator/net.h>
#include <translator/ocr.h>

#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <cmath>

namespace translator {

TranslatorState::TranslatorState() {
    m_worker = std::thread(&TranslatorState::worker_main, this);
}

TranslatorState::~TranslatorState() {
    {
        std::scoped_lock lock(m_mtx);
        m_quit.store(true);
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void TranslatorState::set_options(const Options &opts) {
    std::scoped_lock lock(m_mtx);
    m_opts = opts;
}

Options TranslatorState::get_options() const {
    std::scoped_lock lock(m_mtx);
    return m_opts;
}

void TranslatorState::toggle() {
    const bool now_visible = !m_visible.load();
    m_visible.store(now_visible);
    if (now_visible) {
        std::scoped_lock lock(m_mtx);
        m_request_run = true; // request a translation of the next frame
    }
}

void TranslatorState::set_visible(bool visible) {
    m_visible.store(visible);
}

bool TranslatorState::is_visible() const {
    return m_visible.load();
}

void TranslatorState::set_mode(int mode, uint64_t interval_us) {
    std::scoped_lock lock(m_mtx);
    m_mode = mode;
    if (interval_us > 0)
        m_interval_us = interval_us;
}

bool TranslatorState::wants_new_frame(uint64_t now_us) const {
    if (!m_visible.load())
        return false;

    std::scoped_lock lock(m_mtx);
    if (m_request_run)
        return true;
    if (m_mode == 1) // auto: re-run on interval
        return (now_us - m_last_run_us.load()) >= m_interval_us;
    return false;
}

void TranslatorState::submit_frame(const uint32_t *rgba, uint32_t width,
    uint32_t height, uint64_t now_us) {
    if (!rgba || width == 0 || height == 0)
        return;

    Job job;
    job.width = width;
    job.height = height;
    job.pixels.assign(rgba, rgba + static_cast<size_t>(width) * height);

    {
        std::scoped_lock lock(m_mtx);
        m_pending = std::move(job); // coalesce: keep only the newest frame
        m_request_run = false; // this submission satisfies the request
        m_last_run_us.store(now_us);
    }
    m_cv.notify_all();
}

bool TranslatorState::take_results(std::vector<TranslatedLine> &out) {
    std::scoped_lock lock(m_mtx);
    if (m_generation == m_consumed_generation)
        return false;
    m_consumed_generation = m_generation;
    out = m_results;
    return true;
}

std::string TranslatorState::status_message() const {
    std::scoped_lock lock(m_mtx);
    return m_status;
}

void TranslatorState::set_status(const std::string &msg) {
    std::scoped_lock lock(m_mtx);
    m_status = msg;
}

void TranslatorState::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mtx);
            m_cv.wait(lock, [this] { return m_quit.load() || m_pending.has_value(); });
            if (m_quit.load())
                return;
            job = std::move(*m_pending);
            m_pending.reset();
        }
        run_pipeline(std::move(job.pixels), job.width, job.height);
    }
}

void TranslatorState::run_pipeline(std::vector<uint32_t> pixels, uint32_t width,
    uint32_t height) {
    const Options opts = get_options();

    // 1) OCR
    const auto *rgba = reinterpret_cast<const uint8_t *>(pixels.data());
    std::vector<TextRegion> regions = ocr::recognize(rgba, width, height, opts);
    if (regions.empty()) {
        // Keep previous results; just report nothing found.
        set_status("");
        return;
    }

    // 2) Collect unique, trimmed source strings, separating cache hits/misses.
    std::vector<std::string> miss_texts;
    for (auto &r : regions) {
        r.text = string_utils::trim_copy(r.text);
        if (r.text.empty())
            continue;
        if (m_cache.find(r.text) == m_cache.end()
            && std::find(miss_texts.begin(), miss_texts.end(), r.text) == miss_texts.end())
            miss_texts.push_back(r.text);
    }

    // 3) Translate the misses in a single batch.
    if (!miss_texts.empty()) {
        if (!net::available(opts)) {
            set_status(opts.deepl_api_key.empty()
                    ? "Translation: DeepL API key not set"
                    : "Translation backend unavailable");
            // Fall through: show source text as-is for the misses.
        } else {
            std::vector<std::string> translated = net::translate(miss_texts, opts);
            if (translated.size() == miss_texts.size()) {
                for (size_t i = 0; i < miss_texts.size(); ++i)
                    m_cache[miss_texts[i]] = translated[i];
                set_status("");
            } else {
                set_status("Translation request failed");
            }
        }
    }

    // 4) Build the translated lines with overlay (960x544) coordinates.
    //    Aspect-ratio safety: map normalized coords into the virtual space,
    //    centering with letterbox offsets if the capture aspect differs.
    constexpr float kVW = 960.f;
    constexpr float kVH = 544.f;
    const float cap_ar = static_cast<float>(width) / static_cast<float>(height);
    const float virt_ar = kVW / kVH;
    float sx = kVW, sy = kVH, ox = 0.f, oy = 0.f;
    if (cap_ar > virt_ar) {
        sx = kVH * cap_ar;
        ox = (kVW - sx) / 2.f;
    } else if (cap_ar < virt_ar) {
        sy = kVW / cap_ar;
        oy = (kVH - sy) / 2.f;
    }

    std::vector<TranslatedLine> lines;
    lines.reserve(regions.size());
    for (const auto &r : regions) {
        if (r.text.empty())
            continue;
        TranslatedLine line;
        line.source = r.text;
        auto it = m_cache.find(r.text);
        line.translated = (it != m_cache.end()) ? it->second : r.text;
        line.x = static_cast<int16_t>(std::lround(ox + r.x * sx));
        line.y = static_cast<int16_t>(std::lround(oy + r.y * sy));
        line.w = static_cast<uint16_t>(std::max(0L, std::lround(r.w * sx)));
        line.h = static_cast<uint16_t>(std::max(0L, std::lround(r.h * sy)));
        lines.push_back(std::move(line));
    }

    // 5) Commit results.
    {
        std::scoped_lock lock(m_mtx);
        m_results = std::move(lines);
        ++m_generation;
    }
}

} // namespace translator
