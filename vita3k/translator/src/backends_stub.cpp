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

// Fallback OCR / translation backends for non-Apple platforms. These make the
// screen-translation feature compile and run as a no-op everywhere; native
// backends (Tesseract, raw-socket HTTP) can be added here later.

#include <translator/net.h>
#include <translator/ocr.h>

namespace translator::ocr {

bool available() {
    return false;
}

std::vector<TextRegion> recognize(const uint8_t *, uint32_t, uint32_t,
    const Options &) {
    return {};
}

} // namespace translator::ocr

namespace translator::net {

bool available(const Options &) {
    return false;
}

std::vector<std::string> translate(const std::vector<std::string> &,
    const Options &) {
    return {};
}

} // namespace translator::net
