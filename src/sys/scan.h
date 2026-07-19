#ifndef LISZT_SYS_SCAN_H
#define LISZT_SYS_SCAN_H

#include <stddef.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#define LISZT_SCAN_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define LISZT_SCAN_NEON 1
#endif

/* rank's scan-kernel pattern: inline SIMD bulk steps with scalar
   tails, plus exported scalar reference implementations that serve as
   the fuzz oracles (tests/unit compiles kernel-vs-oracle sweeps). */

size_t liszt_scan_nonascii_scalar(const unsigned char *text, size_t len);
size_t liszt_scan_ascii_graph_scalar(const unsigned char *text,
                                     size_t len);
const char *liszt_scan_backend(void);   /* "sse2" | "neon" | "scalar" */

/* Index of the first byte >= 0x80, or len. The invalid-multibyte gate
   and mb walks skip pure-ASCII prefixes wholesale (ASCII never leaves
   the initial mbstate, so resuming mbrtowc at the boundary is exact). */
static inline size_t
liszt_scan_nonascii(const unsigned char *text, size_t len)
{
    size_t i = 0;

#if defined(LISZT_SCAN_SSE2)
    while (i + 16U <= len) {
        __m128i chunk =
            _mm_loadu_si128((const __m128i *)(const void *)(text + i));
        unsigned int mask = (unsigned int)_mm_movemask_epi8(chunk);

        if (mask != 0)
            return i + (size_t)__builtin_ctz(mask);
        i += 16U;
    }
#elif defined(LISZT_SCAN_NEON)
    while (i + 16U <= len) {
        uint8x16_t chunk = vld1q_u8(text + i);

        if (vmaxvq_u8(chunk) >= 0x80U)
            break;
        i += 16U;
    }
#endif
    for (; i < len; i++)
        if (text[i] >= 0x80U)
            return i;
    return len;
}

/* Index of the first byte outside printable ASCII [0x20,0x7E], or len.
   A maximal printable-ASCII span contributes display width == bytes,
   letting width measurement leap whole spans. */
static inline size_t
liszt_scan_ascii_graph(const unsigned char *text, size_t len)
{
    size_t i = 0;

#if defined(LISZT_SCAN_SSE2)
    if (len >= 16U) {
        const __m128i lo = _mm_set1_epi8(0x20 - 1);
        const __m128i hi = _mm_set1_epi8(0x7F);

        while (i + 16U <= len) {
            __m128i chunk =
                _mm_loadu_si128((const __m128i *)(const void *)(text + i));
            /* Signed compares: bytes >= 0x80 read as negative, which
               correctly fails the "> 0x1F" test. */
            __m128i ok = _mm_and_si128(_mm_cmpgt_epi8(chunk, lo),
                                       _mm_cmplt_epi8(chunk, hi));
            unsigned int bad =
                (unsigned int)_mm_movemask_epi8(ok) ^ 0xFFFFU;

            if (bad != 0)
                return i + (size_t)__builtin_ctz(bad);
            i += 16U;
        }
    }
#elif defined(LISZT_SCAN_NEON)
    const uint8x16_t lo = vdupq_n_u8(0x20);
    const uint8x16_t hi = vdupq_n_u8(0x7E);

    while (i + 16U <= len) {
        uint8x16_t chunk = vld1q_u8(text + i);
        uint8x16_t ok = vandq_u8(vcgeq_u8(chunk, lo),
                                 vcleq_u8(chunk, hi));

        if (vminvq_u8(ok) == 0)
            break;
        i += 16U;
    }
#endif
    for (; i < len; i++)
        if (text[i] < 0x20U || text[i] > 0x7EU)
            return i;
    return len;
}

#endif
