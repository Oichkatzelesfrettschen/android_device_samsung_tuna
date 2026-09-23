/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <utils/Unicode.h>

/*
 * libsec-ril imports strdup8to16() from the libcutils jstring API, which
 * Android 11 no longer ships. The contract: return a malloc()ed UTF-16 copy
 * of the NUL-terminated UTF-8 string s, store its length in UTF-16 code
 * units in *out_len, and return NULL for a NULL or malformed input. The
 * caller releases the buffer with free().
 */
extern "C" char16_t* strdup8to16(const char* s, size_t* out_len) {
    if (s == nullptr) {
        return nullptr;
    }

    const size_t src_len = strlen(s);
    const ssize_t len = utf8_to_utf16_length(reinterpret_cast<const uint8_t*>(s), src_len);
    if (len < 0 || static_cast<size_t>(len) >= SIZE_MAX / sizeof(char16_t)) {
        return nullptr;
    }

    const size_t units = static_cast<size_t>(len) + 1;
    char16_t* ret = static_cast<char16_t*>(malloc(units * sizeof(char16_t)));
    if (ret == nullptr) {
        return nullptr;
    }

    utf8_to_utf16(reinterpret_cast<const uint8_t*>(s), src_len, ret, units);
    if (out_len != nullptr) {
        *out_len = static_cast<size_t>(len);
    }
    return ret;
}
