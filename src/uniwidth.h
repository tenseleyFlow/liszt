#ifndef LISZT_UNIWIDTH_H
#define LISZT_UNIWIDTH_H

#include <stdint.h>

/* gnulib uc_width for the UTF-8 encoding (coreutils 9.11 tables). Used
   as the width backend on platforms whose libc wcwidth fails gnulib's
   conformance probe (LISZT_REPLACE_WCWIDTH), mirroring rpl_wcwidth:
   UTF-8 locales route here, other locales keep libc wcwidth. */
int liszt_uc_width(uint32_t uc);

#endif
