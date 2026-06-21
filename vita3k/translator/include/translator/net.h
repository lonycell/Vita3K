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

// Translation backend interface. The macOS implementation lives in
// net_deepl.mm (DeepL via NSURLSession); other platforms get a stub.

#pragma once

#include <translator/types.h>

#include <string>
#include <vector>

namespace translator::net {

// Translate a batch of texts in a single request. The returned vector has the
// same length and order as the input. On failure an empty vector is returned.
std::vector<std::string> translate(const std::vector<std::string> &texts,
    const Options &opts);

// Whether the translation backend is usable (platform + API key configured).
bool available(const Options &opts);

} // namespace translator::net
