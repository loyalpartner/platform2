// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBBRILLO_BRILLO_SECURE_STRING_H_
#define LIBBRILLO_BRILLO_SECURE_STRING_H_

#include <cstddef>

#include <brillo/asan.h>
#include <brillo/brillo_export.h>

namespace brillo {

// Secure memset(). This function is guaranteed to fill in the whole buffer
// and is not subject to compiler optimization as allowed by Sub-clause 5.1.2.3
// of C Standard [ISO/IEC 9899:2011] which states:
// In the abstract machine, all expressions are evaluated as specified by the
// semantics. An actual implementation need not evaluate part of an expression
// if it can deduce that its value is not used and that no needed side effects
// are produced (including any caused by calling a function or accessing
// a volatile object).
// While memset() can be optimized out in certain situations (since most
// compilers implement this function as intrinsic and know of its side effects),
// this function will not be optimized out.
//
// SecureMemset is used to write beyond the size() in several functions.
// Since this is intentional, disable address sanitizer from analyzing it.
BRILLO_EXPORT BRILLO_DISABLE_ASAN void* SecureMemset(void* v, int c, size_t n);

// Compare [n] bytes starting at [s1] with [s2] and return 0 if they match,
// 1 if they don't. Time taken to perform the comparison is only dependent on
// [n] and not on the relationship of the match between [s1] and [s2].
BRILLO_EXPORT int SecureMemcmp(const void* s1, const void* s2, size_t n);

}  // namespace brillo

#endif  // LIBBRILLO_BRILLO_SECURE_STRING_H_
