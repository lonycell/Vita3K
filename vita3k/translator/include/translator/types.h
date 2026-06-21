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

// Common POD types shared by the screen-translation pipeline.
// See dev-docs/screen-translation/03-detailed-design.md

#pragma once

#include <cstdint>
#include <string>

namespace translator {

// A single recognized text line returned by the OCR backend.
// Coordinates are normalized to [0, 1] with origin at the TOP-LEFT of the
// captured image (the backend is responsible for flipping any bottom-left
// coordinate spaces, e.g. Apple Vision).
struct TextRegion {
    float x = 0.f; // left
    float y = 0.f; // top
    float w = 0.f; // width
    float h = 0.f; // height
    std::string text; // recognized source text (UTF-8)
    float confidence = 0.f;
};

// A fully translated line ready to be placed on the overlay.
// Coordinates are in the overlay virtual coordinate space (960 x 544).
struct TranslatedLine {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    std::string source; // original text (UTF-8)
    std::string translated; // translated text (UTF-8)
};

// Runtime options, populated from the emulator Config.
struct Options {
    // DeepL source language code. Empty string => let DeepL auto-detect.
    std::string source_lang = "JA";
    std::string target_lang = "KO";
    std::string deepl_api_key;
    bool deepl_free_tier = true; // api-free.deepl.com vs api.deepl.com
    float min_confidence = 0.3f; // drop OCR lines below this confidence
};

} // namespace translator
