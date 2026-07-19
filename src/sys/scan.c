#include "sys/scan.h"

/* Scalar reference implementations: the oracles the unit fuzz sweeps
   compare the inline kernels against, byte-for-byte. */

size_t
liszt_scan_nonascii_scalar(const unsigned char *text, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (text[i] >= 0x80U)
            return i;
    return len;
}

size_t
liszt_scan_ascii_graph_scalar(const unsigned char *text, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (text[i] < 0x20U || text[i] > 0x7EU)
            return i;
    return len;
}

const char *
liszt_scan_backend(void)
{
#if defined(LISZT_SCAN_SSE2)
    return "sse2";
#elif defined(LISZT_SCAN_NEON)
    return "neon";
#else
    return "scalar";
#endif
}
