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

// Transparent overlay that draws translated text on top of the running game.
// Lines are positioned in the overlay virtual coordinate space (960 x 544).
// See dev-docs/screen-translation/03-detailed-design.md

#pragma once

#include <overlay/controls.h>
#include <overlay/overlay.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace overlay {

struct translation_overlay : public overlay {
    translation_overlay();

    struct line {
        int16_t x = 0;
        int16_t y = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        std::u32string text;
    };

    // Thread-safe setters (called from the glue thread).
    void set_lines(std::vector<line> lines);
    void set_status(std::u32string status);
    void clear();

    compiled_resource get_compiled() override;

private:
    std::mutex m_mtx;
    std::vector<line> m_lines;
    std::u32string m_status;

    static constexpr uint16_t k_font_size = 18;
    static constexpr uint16_t k_min_font_size = 11;
    static constexpr uint16_t k_padding = 4;
    static constexpr float k_bg_opacity = 0.72f;
};

} // namespace overlay
