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

// DeepL translation backend (macOS, NSURLSession). Built with -fobjc-arc.

#include <translator/net.h>

#import <Foundation/Foundation.h>

namespace translator::net {

bool available(const Options &opts) {
    return !opts.deepl_api_key.empty();
}

// Percent-encode a string for application/x-www-form-urlencoded.
static NSString *form_encode(NSString *value) {
    NSMutableCharacterSet *allowed =
        [NSMutableCharacterSet alphanumericCharacterSet];
    [allowed addCharactersInString:@"-._~"];
    return [value stringByAddingPercentEncodingWithAllowedCharacters:allowed];
}

std::vector<std::string> translate(const std::vector<std::string> &texts,
    const Options &opts) {
    std::vector<std::string> result;
    if (texts.empty() || opts.deepl_api_key.empty())
        return result;

    @autoreleasepool {
        NSString *endpoint = opts.deepl_free_tier
            ? @"https://api-free.deepl.com/v2/translate"
            : @"https://api.deepl.com/v2/translate";

        // Build the urlencoded body with one text= per source string.
        NSMutableString *body = [NSMutableString string];
        for (const auto &t : texts) {
            NSString *s = [NSString stringWithUTF8String:t.c_str()];
            if (!s)
                s = @"";
            [body appendFormat:@"text=%@&", form_encode(s)];
        }
        if (!opts.target_lang.empty()) {
            [body appendFormat:@"target_lang=%@&",
                form_encode([NSString stringWithUTF8String:opts.target_lang.c_str()])];
        }
        if (!opts.source_lang.empty()) {
            [body appendFormat:@"source_lang=%@",
                form_encode([NSString stringWithUTF8String:opts.source_lang.c_str()])];
        }

        NSMutableURLRequest *request =
            [NSMutableURLRequest requestWithURL:[NSURL URLWithString:endpoint]];
        request.HTTPMethod = @"POST";
        NSString *auth = [NSString stringWithFormat:@"DeepL-Auth-Key %s",
                                   opts.deepl_api_key.c_str()];
        [request setValue:auth forHTTPHeaderField:@"Authorization"];
        [request setValue:@"application/x-www-form-urlencoded"
            forHTTPHeaderField:@"Content-Type"];
        request.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];
        request.timeoutInterval = 10.0;

        __block NSData *resp_data = nil;
        __block NSInteger status_code = 0;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        NSURLSessionDataTask *task = [[NSURLSession sharedSession]
            dataTaskWithRequest:request
              completionHandler:^(NSData *data, NSURLResponse *response,
                  NSError *error) {
                  if (!error && data) {
                      resp_data = data;
                      if ([response isKindOfClass:[NSHTTPURLResponse class]])
                          status_code = [(NSHTTPURLResponse *)response statusCode];
                  }
                  dispatch_semaphore_signal(sem);
              }];
        [task resume];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
                                         (int64_t)(12.0 * NSEC_PER_SEC)));

        if (!resp_data || status_code < 200 || status_code >= 300)
            return result;

        NSError *json_error = nil;
        NSDictionary *json = [NSJSONSerialization JSONObjectWithData:resp_data
                                                            options:0
                                                              error:&json_error];
        if (json_error || ![json isKindOfClass:[NSDictionary class]])
            return result;

        NSArray *translations = json[@"translations"];
        if (![translations isKindOfClass:[NSArray class]])
            return result;

        result.reserve(texts.size());
        for (NSDictionary *t in translations) {
            NSString *text = [t isKindOfClass:[NSDictionary class]] ? t[@"text"] : nil;
            result.push_back(text ? std::string(text.UTF8String) : std::string());
        }
    }

    return result;
}

} // namespace translator::net
