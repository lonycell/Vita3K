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

// Apple Vision OCR backend (macOS). Built with -fobjc-arc.

#include <translator/ocr.h>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

namespace translator::ocr {

bool available() {
    return true;
}

// Map a DeepL-style source language code (e.g. "JA") to Vision recognition
// language identifiers, most-specific first. An empty list lets Vision decide.
static NSArray<NSString *> *recognition_languages(const std::string &src) {
    if (src == "JA" || src == "ja")
        return @[ @"ja-JP", @"ja" ];
    if (src == "ZH" || src == "zh")
        return @[ @"zh-Hans", @"zh-Hant" ];
    if (src == "KO" || src == "ko")
        return @[ @"ko-KR", @"ko" ];
    if (src == "EN" || src == "en")
        return @[ @"en-US" ];
    if (!src.empty())
        return @[ [NSString stringWithUTF8String:src.c_str()] ];
    return @[];
}

std::vector<TextRegion> recognize(const uint8_t *rgba, uint32_t width,
    uint32_t height, const Options &opts) {
    std::vector<TextRegion> out;
    if (!rgba || width == 0 || height == 0)
        return out;

    @autoreleasepool {
        CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
        if (!color_space)
            return out;

        // Bytes are laid out R,G,B,A (matching the screenshot path). We ignore
        // the alpha channel for recognition.
        const size_t bytes_per_row = static_cast<size_t>(width) * 4;
        CGBitmapInfo bitmap_info = kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault;
        CGContextRef ctx = CGBitmapContextCreate(
            const_cast<uint8_t *>(rgba), width, height, 8, bytes_per_row,
            color_space, bitmap_info);
        CGColorSpaceRelease(color_space);
        if (!ctx)
            return out;

        CGImageRef cg_image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        if (!cg_image)
            return out;

        VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
        request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        request.usesLanguageCorrection = YES;
        NSArray<NSString *> *langs = recognition_languages(opts.source_lang);
        if (langs.count > 0)
            request.recognitionLanguages = langs;

        VNImageRequestHandler *handler =
            [[VNImageRequestHandler alloc] initWithCGImage:cg_image options:@{}];
        CGImageRelease(cg_image);

        NSError *error = nil;
        if (![handler performRequests:@[ request ] error:&error] || error)
            return out;

        for (VNRecognizedTextObservation *obs in request.results) {
            VNRecognizedText *best = [[obs topCandidates:1] firstObject];
            if (!best)
                continue;
            if (best.confidence < opts.min_confidence)
                continue;

            const char *utf8 = best.string.UTF8String;
            if (!utf8 || utf8[0] == '\0')
                continue;

            // Vision bounding boxes are normalized with a BOTTOM-LEFT origin.
            // Flip Y so our TextRegion uses a TOP-LEFT origin.
            const CGRect bb = obs.boundingBox;
            TextRegion region;
            region.x = static_cast<float>(bb.origin.x);
            region.y = static_cast<float>(1.0 - (bb.origin.y + bb.size.height));
            region.w = static_cast<float>(bb.size.width);
            region.h = static_cast<float>(bb.size.height);
            region.text = utf8;
            region.confidence = best.confidence;
            out.push_back(std::move(region));
        }
    }

    return out;
}

} // namespace translator::ocr
