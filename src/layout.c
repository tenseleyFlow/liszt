#include "layout.h"

#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "util.h"

enum { MIN_COLUMN_WIDTH = 3 };

/* GNU's column_info: per candidate column-count, the running line
   length, per-column width array, and validity. The col_arr storage is
   one triangular allocation, exactly as init_column_info builds it. */
struct column_info {
    bool valid_len;
    size_t line_len;
    size_t *col_arr;
};

static struct column_info *column_info;
static size_t column_info_alloc;
static size_t *col_arr_store;
static size_t col_arr_alloc;

static void
init_column_info(size_t max_cols)
{
    if (column_info_alloc < max_cols) {
        column_info = liszt_xrealloc(column_info,
                                     max_cols * sizeof *column_info);
        column_info_alloc = max_cols;
    }
    size_t cells = max_cols * (max_cols + 1) / 2;
    if (col_arr_alloc < cells) {
        col_arr_store = liszt_xrealloc(col_arr_store,
                                       cells * sizeof *col_arr_store);
        col_arr_alloc = cells;
    }
    size_t *p = col_arr_store;
    for (size_t i = 0; i < max_cols; i++) {
        column_info[i].valid_len = true;
        column_info[i].line_len = (i + 1) * MIN_COLUMN_WIDTH;
        column_info[i].col_arr = p;
        p += i + 1;
        for (size_t j = 0; j <= i; j++)
            column_info[i].col_arr[j] = MIN_COLUMN_WIDTH;
    }
}

static size_t
calculate_columns(size_t n, const size_t *lengths, bool by_columns,
                  size_t line_length, size_t max_idx)
{
    size_t max_cols = (0 < max_idx && max_idx < n) ? max_idx : n;

    init_column_info(max_cols);

    for (size_t filesno = 0; filesno < n; filesno++) {
        size_t name_length = lengths[filesno];
        for (size_t i = 0; i < max_cols; i++) {
            if (column_info[i].valid_len) {
                size_t idx = by_columns
                    ? filesno / ((n + i) / (i + 1))
                    : filesno % (i + 1);
                size_t real_length = name_length + (idx == i ? 0 : 2);

                if (column_info[i].col_arr[idx] < real_length) {
                    column_info[i].line_len +=
                        real_length - column_info[i].col_arr[idx];
                    column_info[i].col_arr[idx] = real_length;
                    column_info[i].valid_len =
                        column_info[i].line_len < line_length;
                }
            }
        }
    }

    size_t cols;
    for (cols = max_cols; 1 < cols; --cols)
        if (column_info[cols - 1].valid_len)
            break;
    return cols;
}

static void
indent(size_t from, size_t to, size_t tabsize)
{
    while (from < to) {
        if (tabsize != 0 && to / tabsize > (from + 1) / tabsize) {
            liszt_emit_byte('\t');
            from += tabsize - from % tabsize;
        } else {
            liszt_emit_byte(' ');
            from++;
        }
    }
}

void
liszt_layout_columns(size_t n, const size_t *lengths, bool by_columns,
                     size_t line_length, size_t max_idx, size_t tabsize,
                     liszt_layout_emit emit, void *ctx)
{
    if (n == 0)
        return;

    size_t cols = calculate_columns(n, lengths, by_columns, line_length,
                                    max_idx);
    const size_t *col_arr = column_info[cols - 1].col_arr;

    if (by_columns) {
        size_t rows = n / cols + (n % cols != 0);

        for (size_t row = 0; row < rows; row++) {
            size_t col = 0;
            size_t filesno = row;
            size_t pos = 0;

            for (;;) {
                size_t name_length = lengths[filesno];
                size_t max_name_length = col_arr[col++];

                emit(filesno, pos, ctx);
                if (n - rows <= filesno)
                    break;
                filesno += rows;
                indent(pos + name_length, pos + max_name_length, tabsize);
                pos += max_name_length;
            }
            liszt_emit_byte('\n');
        }
    } else {
        size_t pos = 0;
        size_t name_length = lengths[0];
        size_t max_name_length = col_arr[0];

        emit(0, 0, ctx);
        for (size_t filesno = 1; filesno < n; filesno++) {
            size_t col = filesno % cols;

            if (col == 0) {
                liszt_emit_byte('\n');
                pos = 0;
            } else {
                indent(pos + name_length, pos + max_name_length, tabsize);
                pos += max_name_length;
            }
            emit(filesno, pos, ctx);
            name_length = lengths[filesno];
            max_name_length = col_arr[col];
        }
        liszt_emit_byte('\n');
    }
}

void
liszt_layout_separated(size_t n, const size_t *lengths, char sep,
                       size_t line_length, liszt_layout_emit emit,
                       void *ctx)
{
    size_t pos = 0;

    if (n == 0)
        return;
    for (size_t filesno = 0; filesno < n; filesno++) {
        size_t len = line_length ? lengths[filesno] : 0;

        if (filesno != 0) {
            char separator;

            if (!line_length
                || (pos + len + 2 < line_length
                    && pos <= (size_t)-1 - len - 2)) {
                pos += 2;
                separator = ' ';
            } else {
                pos = 0;
                separator = '\n';
            }
            liszt_emit_byte(sep);
            liszt_emit_byte(separator);
        }
        emit(filesno, pos, ctx);
        pos += len;
    }
    liszt_emit_byte('\n');
}
