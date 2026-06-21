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

// OCR backend interface. The macOS implementation lives in ocr_vision.mm
// (Apple Vision); other platforms get a stub in backends_stub.cpp.

#pragma once

#include <translator/types.h>

#include <cstdint>
#include <vector>

namespace translator::ocr {

// Recognize text in an RGBA8 image (row-major, stride = width * 4, top-left
// origin). Returns recognized lines with TOP-LEFT normalized coordinates.
// Returns an empty vector on failure or when no text is found.
std::vector<TextRegion> recognize(const uint8_t *rgba, uint32_t width,
    uint32_t height, const Options &opts);

// Whether OCR is usable in this build / on this platform.
bool available();

} // namespace translator::ocr
