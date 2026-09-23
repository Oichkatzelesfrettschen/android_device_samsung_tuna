/* SPDX-License-Identifier: Apache-2.0 */

/*
 * libsec-ril imports android_atomic_release_cas(), which libcutils exported
 * out of line until its atomics became header-only static inline functions.
 * Defining ANDROID_ATOMIC_INLINE empty before including <cutils/atomic.h>
 * emits the android_atomic_* family as ordinary exported functions again,
 * with the header's memory-order semantics.
 */
#define ANDROID_ATOMIC_INLINE
#include <cutils/atomic.h>
