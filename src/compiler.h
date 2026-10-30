/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/*
 * compiler.h - Compiler portability helpers (internal).
 *
 * Wraps compiler-specific attributes and pragmas behind CHIPS_* macros.
 * Unsupported compilers get no-op fallbacks: code stays correct, only
 * optimisation hints are lost.
 *
 * Not part of the public API; include from implementation files only.
 */

#ifndef CHIPS_COMPILER_H
#define CHIPS_COMPILER_H

#if defined(__GNUC__) || defined(__clang__)
#define CHIPS_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define CHIPS_ALWAYS_INLINE inline
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CHIPS_NOINLINE __attribute__((noinline))
#else
#define CHIPS_NOINLINE
#endif

#define CHIPS_PRAGMA_(x) _Pragma(#x)
#define CHIPS_PRAGMA(x) CHIPS_PRAGMA_(x)

#if defined(__clang__)
#define CHIPS_UNROLL_FULL CHIPS_PRAGMA(clang loop unroll(full))
#elif defined(__GNUC__) && (__GNUC__ >= 8)
#define CHIPS_UNROLL_FULL CHIPS_PRAGMA(GCC unroll 65534)
#else
#define CHIPS_UNROLL_FULL
#endif

#endif /* CHIPS_COMPILER_H */
