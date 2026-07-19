#ifndef LISZT_LAYOUT_H
#define LISZT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/* GNU ls's column machinery (ls.c calculate_columns/init_column_info/
   print_many_per_line/print_horizontal/print_with_separator/indent),
   ported to work over an array of display lengths with emission through
   a callback. All output flows through the emit module. */

typedef void (*liszt_layout_emit)(size_t idx, size_t start_col, void *ctx);

/* -C (by_columns=true) and -x (false). LINE_LENGTH 0 means unlimited -
   the caller handles that case via liszt_layout_separated with ' '. */
void liszt_layout_columns(size_t n, const size_t *lengths, bool by_columns,
                          size_t line_length, size_t max_idx,
                          size_t tabsize, liszt_layout_emit emit,
                          void *ctx);

/* -m (sep=',') and the zero-line-length variant of -C/-x (sep=' '). */
void liszt_layout_separated(size_t n, const size_t *lengths, char sep,
                            size_t line_length, liszt_layout_emit emit,
                            void *ctx);

#endif
