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

#include <overlay/translation_overlay.h>

#include <overlay/font.h>
#include <overlay/types.h>

#include <algorithm>
#include <cmath>

namespace overlay {

translation_overlay::translation_overlay() {
    visible = false;
}

void translation_overlay::set_lines(std::vector<line> lines) {
    {
        std::scoped_lock lock(m_mtx);
        m_lines = std::move(lines);
    }
    needs_redraw = true;
    refresh();
}

void translation_overlay::set_status(std::u32string status) {
    {
        std::scoped_lock lock(m_mtx);
        if (m_status == status)
            return;
        m_status = std::move(status);
    }
    needs_redraw = true;
    refresh();
}

void translation_overlay::clear() {
    {
        std::scoped_lock lock(m_mtx);
        m_lines.clear();
        m_status.clear();
    }
    needs_redraw = true;
    refresh();
}

compiled_resource translation_overlay::get_compiled() {
    compiled_resource result;
    if (!visible)
        return result;

    std::vector<line> lines;
    std::u32string status;
    {
        std::scoped_lock lock(m_mtx);
        lines = m_lines;
        status = m_status;
    }

    for (const auto &ln : lines) {
        if (ln.text.empty())
            continue;

        // Scale the font to the original text-box height; the label's own
        // back_color provides the semi-transparent backing for readability.
        uint16_t font_size = k_font_size;
        if (ln.h > 0) {
            const long scaled = std::lround(static_cast<float>(ln.h) / 1.35f);
            font_size = static_cast<uint16_t>(
                std::clamp<long>(scaled, k_min_font_size, 28));
        }

        // Wrap to roughly the original width (clamped to the screen).
        uint16_t wrap_w = std::max<uint16_t>(ln.w, static_cast<uint16_t>(font_size * 4));
        const int16_t max_w = static_cast<int16_t>(virtual_width - ln.x - k_padding);
        if (max_w > 0)
            wrap_w = std::min<uint16_t>(wrap_w, static_cast<uint16_t>(max_w));

        label lbl;
        lbl.set_font(default_font_name, font_size);
        lbl.fore_color = { 1.f, 1.f, 1.f, 1.f };
        lbl.back_color = { 0.f, 0.f, 0.f, k_bg_opacity };
        lbl.set_padding(k_padding);
        lbl.set_wrap_text(true);
        lbl.set_unicode_text(ln.text);
        lbl.set_size(wrap_w, 0);
        lbl.auto_resize(false, wrap_w, virtual_height);

        int16_t pos_x = ln.x;
        int16_t pos_y = ln.y;
        // Keep the box on-screen vertically.
        if (pos_y + lbl.h > virtual_height)
            pos_y = static_cast<int16_t>(std::max(0, virtual_height - lbl.h));
        lbl.set_pos(pos_x, pos_y);

        result.add(lbl.get_compiled());
    }

    if (!status.empty()) {
        label toast;
        toast.set_font(default_font_name, 14);
        toast.fore_color = { 1.f, 1.f, 0.6f, 1.f };
        toast.back_color = { 0.f, 0.f, 0.f, 0.8f };
        toast.set_padding(6);
        toast.set_unicode_text(status);
        toast.auto_resize();
        const int16_t tx = static_cast<int16_t>(std::max(0, (virtual_width - toast.w) / 2));
        toast.set_pos(tx, 8);
        result.add(toast.get_compiled());
    }

    return result;
}

} // namespace overlay
