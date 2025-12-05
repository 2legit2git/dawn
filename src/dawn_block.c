// dawn_block.c

#include "dawn_block.h"
#include "dawn_gap.h"
#include "dawn_wrap.h"
#include "dawn_tex.h"
#include "dawn_image.h"

// Forward declarations for internal helpers
static Block *block_cache_add(BlockCache *bc);
static void block_free(Block *block);
static bool is_at_line_start(const GapBuffer *gb, size_t pos);
static size_t find_line_end(const GapBuffer *gb, size_t pos);
static bool try_parse_image(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_code_block(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_block_math(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_table(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_hr(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_header(Block *block, const GapBuffer *gb, size_t pos, int wrap_width);
static bool try_parse_footnote_def(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_blockquote(Block *block, const GapBuffer *gb, size_t pos);
static bool try_parse_list_item(Block *block, const GapBuffer *gb, size_t pos);
static void parse_paragraph(Block *block, const GapBuffer *gb, size_t pos, int wrap_width);
static int calculate_block_vrows(const Block *block, const GapBuffer *gb, int wrap_width, int text_height);
static bool is_block_start(const GapBuffer *gb, size_t pos);

// #region Block Cache Management

void block_cache_init(BlockCache *bc) {
    bc->blocks = NULL;
    bc->count = 0;
    bc->capacity = 0;
    bc->text_len = 0;
    bc->total_vrows = 0;
    bc->valid = false;
    bc->wrap_width = 0;
}

void block_cache_free(BlockCache *bc) {
    if (bc->blocks) {
        for (int i = 0; i < bc->count; i++) {
            block_free(&bc->blocks[i]);
        }
        free(bc->blocks);
    }
    bc->blocks = NULL;
    bc->count = 0;
    bc->capacity = 0;
    bc->valid = false;
}

void block_cache_invalidate(BlockCache *bc) {
    // Free cached resources but keep structure
    if (bc->blocks) {
        for (int i = 0; i < bc->count; i++) {
            block_free(&bc->blocks[i]);
        }
    }
    bc->count = 0;
    bc->valid = false;
}

//! Add a new block to the cache, growing capacity if needed
static Block *block_cache_add(BlockCache *bc) {
    if (bc->count >= bc->capacity) {
        int new_capacity = bc->capacity == 0 ? BLOCK_CACHE_INITIAL_CAPACITY : bc->capacity * 2;
        Block *new_blocks = realloc(bc->blocks, sizeof(Block) * (size_t)new_capacity);
        if (!new_blocks) return NULL;
        bc->blocks = new_blocks;
        bc->capacity = new_capacity;
    }

    Block *block = &bc->blocks[bc->count++];
    memset(block, 0, sizeof(Block));
    return block;
}

//! Free resources owned by a block
static void block_free(Block *block) {
    switch (block->type) {
        case BLOCK_CODE:
            free(block->data.code.highlighted);
            block->data.code.highlighted = NULL;
            break;

        case BLOCK_MATH:
            if (block->data.math.tex_sketch) {
                tex_sketch_free(block->data.math.tex_sketch);
                block->data.math.tex_sketch = NULL;
            }
            break;

        case BLOCK_IMAGE:
            free(block->data.image.resolved_path);
            block->data.image.resolved_path = NULL;
            break;

        case BLOCK_PARAGRAPH:
            block_free_inline_runs(block);
            break;

        default:
            break;
    }
}

// #endregion

// #region Block Parsing

void block_cache_parse(BlockCache *bc, const GapBuffer *gb, int wrap_width, int text_height) {
    // Clear existing blocks
    block_cache_invalidate(bc);

    bc->text_len = gap_len(gb);
    bc->wrap_width = wrap_width;
    bc->text_height = text_height;
    bc->total_vrows = 0;

    size_t pos = 0;
    size_t len = bc->text_len;

    while (pos < len) {
        Block *block = block_cache_add(bc);
        if (!block) break;  // Out of memory

        block->start = pos;
        block->vrow_start = bc->total_vrows;

        // Try each block type in priority order
        if (try_parse_image(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_code_block(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_block_math(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_table(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_hr(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_header(block, gb, pos, wrap_width)) {
            pos = block->end;
        }
        else if (try_parse_footnote_def(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_blockquote(block, gb, pos)) {
            pos = block->end;
        }
        else if (try_parse_list_item(block, gb, pos)) {
            pos = block->end;
        }
        else {
            // Default: paragraph (extends to blank line or block element)
            parse_paragraph(block, gb, pos, wrap_width);
            pos = block->end;
        }

        // Calculate virtual rows for this block
        block->vrow_count = calculate_block_vrows(block, gb, wrap_width, text_height);
        bc->total_vrows += block->vrow_count;
    }

    bc->valid = true;
}

// #endregion

// #region Block Detection Helpers

//! Check if position is at the start of a line
static bool is_at_line_start(const GapBuffer *gb, size_t pos) {
    if (pos == 0) return true;
    return gap_at(gb, pos - 1) == '\n';
}

//! Find end of line (newline position or end of buffer)
static size_t find_line_end(const GapBuffer *gb, size_t pos) {
    size_t len = gap_len(gb);
    while (pos < len && gap_at(gb, pos) != '\n') {
        pos++;
    }
    return pos;
}

//! Check if position starts a block element
static bool is_block_start(const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t len = gap_len(gb);
    if (pos >= len) return false;

    char c = gap_at(gb, pos);

    // Image: ![
    if (c == '!' && pos + 1 < len && gap_at(gb, pos + 1) == '[') return true;

    // Code fence: ```
    if (c == '`' && pos + 2 < len &&
        gap_at(gb, pos + 1) == '`' && gap_at(gb, pos + 2) == '`') return true;

    // Block math: $$
    if (c == '$' && pos + 1 < len && gap_at(gb, pos + 1) == '$') return true;

    // Table: |
    if (c == '|') return true;

    // HR: ---, ***, ___
    if (c == '-' || c == '*' || c == '_') {
        size_t rule_len;
        if (md_check_hr(gb, pos, &rule_len)) return true;
    }

    // Header: #
    if (c == '#') return true;

    // Footnote definition: [^
    if (c == '[' && pos + 1 < len && gap_at(gb, pos + 1) == '^') return true;

    // Blockquote: >
    if (c == '>') return true;

    // List: -, *, +
    if (c == '-' || c == '*' || c == '+') {
        if (pos + 1 < len && gap_at(gb, pos + 1) == ' ') return true;
    }

    // Ordered list: digit followed by . or )
    if (c >= '0' && c <= '9') {
        size_t p = pos;
        while (p < len && gap_at(gb, p) >= '0' && gap_at(gb, p) <= '9') p++;
        if (p < len && (gap_at(gb, p) == '.' || gap_at(gb, p) == ')')) {
            if (p + 1 < len && gap_at(gb, p + 1) == ' ') return true;
        }
    }

    return false;
}

// #endregion

// #region Block Type Parsers

static bool try_parse_image(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t alt_start, alt_len, path_start, path_len, total_len;
    int img_width, img_height;

    if (!md_check_image(gb, pos, &alt_start, &alt_len, &path_start, &path_len,
                        &img_width, &img_height, &total_len)) {
        return false;
    }

    // For block images, the image must be alone on its line
    // Check that after the image syntax there's only whitespace until newline/EOF
    size_t check_pos = pos + total_len;
    size_t len = gap_len(gb);
    while (check_pos < len && gap_at(gb, check_pos) == ' ') {
        check_pos++;
    }
    // If there's non-whitespace text after the image (before newline), it's not a block image
    if (check_pos < len && gap_at(gb, check_pos) != '\n') {
        return false;
    }

    block->type = BLOCK_IMAGE;
    block->end = pos + total_len;

    // Include trailing whitespace and newline
    while (block->end < len && gap_at(gb, block->end) == ' ') {
        block->end++;
    }
    if (block->end < len && gap_at(gb, block->end) == '\n') {
        block->end++;
    }

    block->data.image.alt_start = alt_start;
    block->data.image.alt_len = alt_len;
    block->data.image.path_start = path_start;
    block->data.image.path_len = path_len;
    block->data.image.width = img_width;
    block->data.image.height = img_height;
    block->data.image.display_rows = 0;  // Calculated later
    block->data.image.resolved_path = NULL;

    return true;
}

static bool try_parse_code_block(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t lang_start, lang_len, content_start, content_len, total_len;

    if (!md_check_code_block(gb, pos, &lang_start, &lang_len,
                             &content_start, &content_len, &total_len)) {
        return false;
    }

    block->type = BLOCK_CODE;
    block->end = pos + total_len;

    block->data.code.lang_start = lang_start;
    block->data.code.lang_len = lang_len;
    block->data.code.content_start = content_start;
    block->data.code.content_len = content_len;
    block->data.code.highlighted = NULL;
    block->data.code.highlighted_len = 0;

    return true;
}

static bool try_parse_block_math(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t content_start, content_len, total_len;

    if (!md_check_block_math_full(gb, pos, &content_start, &content_len, &total_len)) {
        return false;
    }

    block->type = BLOCK_MATH;
    block->end = pos + total_len;

    block->data.math.content_start = content_start;
    block->data.math.content_len = content_len;
    block->data.math.tex_sketch = NULL;

    return true;
}

static bool try_parse_table(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    MdTable tbl;
    if (!md_check_table(gb, pos, &tbl)) {
        return false;
    }

    block->type = BLOCK_TABLE;
    block->end = pos + tbl.total_len;

    block->data.table.col_count = tbl.col_count;
    block->data.table.row_count = tbl.row_count;
    memcpy(block->data.table.align, tbl.align, sizeof(tbl.align));

    return true;
}

static bool try_parse_hr(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t rule_len;
    if (!md_check_hr(gb, pos, &rule_len)) {
        return false;
    }

    block->type = BLOCK_HR;
    block->end = pos + rule_len;

    // Include trailing newline
    if (block->end < gap_len(gb) && gap_at(gb, block->end) == '\n') {
        block->end++;
    }

    block->data.hr.rule_len = rule_len;

    return true;
}

static bool try_parse_header(Block *block, const GapBuffer *gb, size_t pos, int wrap_width) {
    if (!is_at_line_start(gb, pos)) return false;

    MdStyle header_style = md_check_header(gb, pos);
    if (!header_style) return false;

    size_t content_start;
    int level = md_check_header_content(gb, pos, &content_start);
    if (level == 0) return false;

    block->type = BLOCK_HEADER;

    // Find end of header line
    size_t end = find_line_end(gb, pos);
    block->end = end;

    // Include trailing newline
    if (block->end < gap_len(gb) && gap_at(gb, block->end) == '\n') {
        block->end++;
    }

    block->data.header.level = level;
    block->data.header.content_start = content_start;

    // Check for heading ID {#id}
    size_t id_start, id_len, id_total;
    if (md_check_heading_id(gb, content_start, &id_start, &id_len, &id_total)) {
        block->data.header.id_start = id_start;
        block->data.header.id_len = id_len;
    } else {
        block->data.header.id_start = 0;
        block->data.header.id_len = 0;
    }

    (void)wrap_width;  // Will be used in vrow calculation
    return true;
}

static bool try_parse_footnote_def(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t id_start, id_len, content_start, total_len;
    if (!md_check_footnote_def(gb, pos, &id_start, &id_len, &content_start, &total_len)) {
        return false;
    }

    block->type = BLOCK_FOOTNOTE_DEF;

    // Find end of footnote (ends at blank line or next footnote def)
    size_t len = gap_len(gb);
    size_t end = content_start;

    while (end < len) {
        // Find end of current line
        while (end < len && gap_at(gb, end) != '\n') end++;

        // Check if this is end of buffer
        if (end >= len) break;

        // Move past newline
        end++;

        // Check if next line is blank or another footnote def
        if (end < len) {
            if (gap_at(gb, end) == '\n') break;  // Blank line
            size_t d1, d2, d3, d4;
            if (md_check_footnote_def(gb, end, &d1, &d2, &d3, &d4)) break;  // Another def
        }
    }

    block->end = end;
    block->data.footnote.id_start = id_start;
    block->data.footnote.id_len = id_len;
    block->data.footnote.content_start = content_start;

    return true;
}

static bool try_parse_blockquote(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t content_start;
    int level = md_check_blockquote(gb, pos, &content_start);
    if (level == 0) return false;

    block->type = BLOCK_BLOCKQUOTE;

    // Find end of blockquote (continues while lines start with >)
    size_t len = gap_len(gb);
    size_t end = find_line_end(gb, pos);

    while (end < len) {
        // Move past newline
        if (gap_at(gb, end) == '\n') end++;

        // Check if next line continues blockquote
        if (end < len && gap_at(gb, end) == '>') {
            end = find_line_end(gb, end);
        } else {
            break;
        }
    }

    block->end = end;
    block->data.quote.level = level;
    block->data.quote.content_start = content_start;

    return true;
}

static bool try_parse_list_item(Block *block, const GapBuffer *gb, size_t pos) {
    if (!is_at_line_start(gb, pos)) return false;

    size_t content_start;
    int indent;

    // Check for task list first
    int task_state = md_check_task(gb, pos, &content_start, &indent);
    if (task_state > 0) {
        block->type = BLOCK_LIST_ITEM;
        block->end = find_line_end(gb, pos);
        if (block->end < gap_len(gb) && gap_at(gb, block->end) == '\n') {
            block->end++;
        }
        block->data.list.list_type = 1;  // Task lists are unordered
        block->data.list.indent = indent;
        block->data.list.task_state = task_state;
        block->data.list.content_start = content_start;
        return true;
    }

    // Check for regular list
    int list_type = md_check_list(gb, pos, &content_start, &indent);
    if (list_type == 0) return false;

    block->type = BLOCK_LIST_ITEM;
    block->end = find_line_end(gb, pos);
    if (block->end < gap_len(gb) && gap_at(gb, block->end) == '\n') {
        block->end++;
    }

    block->data.list.list_type = list_type;
    block->data.list.indent = indent;
    block->data.list.task_state = 0;
    block->data.list.content_start = content_start;

    return true;
}

static void parse_paragraph(Block *block, const GapBuffer *gb, size_t pos, int wrap_width) {
    block->type = BLOCK_PARAGRAPH;
    block->data.paragraph.runs = NULL;
    block->data.paragraph.run_count = 0;
    block->data.paragraph.run_capacity = 0;

    size_t len = gap_len(gb);
    size_t end = pos;

    while (end < len) {
        char c = gap_at(gb, end);

        if (c == '\n') {
            // Check for blank line (paragraph end)
            if (end + 1 < len && gap_at(gb, end + 1) == '\n') {
                end++;  // Include first newline
                break;
            }

            // Check if next line starts a block element
            if (end + 1 < len && is_block_start(gb, end + 1)) {
                break;
            }
        }
        end++;
    }

    // Include trailing newline if present
    if (end < len && gap_at(gb, end) == '\n') {
        end++;
    }

    block->end = end;

    // Parse inline runs eagerly
    block_parse_inline_runs(block, gb);

    (void)wrap_width;  // Used in vrow calculation
}

// #endregion

// #region Virtual Row Calculation

static int calculate_block_vrows(const Block *block, const GapBuffer *gb, int wrap_width, int text_height) {
    if (wrap_width <= 0) wrap_width = 80;
    if (text_height <= 0) text_height = 24;

    switch (block->type) {
        case BLOCK_HR:
            return 1;

        case BLOCK_IMAGE: {
            // Use cached display_rows if already calculated
            if (block->data.image.display_rows > 0) {
                return block->data.image.display_rows;
            }

            // Calculate image rows from dimensions
            int img_w = block->data.image.width;
            int img_h = block->data.image.height;

            // Extract raw path
            char raw_path[512];
            size_t plen = block->data.image.path_len;
            if (plen > sizeof(raw_path) - 1) plen = sizeof(raw_path) - 1;
            for (size_t i = 0; i < plen; i++) {
                raw_path[i] = gap_at(gb, block->data.image.path_start + i);
            }
            raw_path[plen] = '\0';

            // Resolve and cache the image (handles URLs, relative paths, etc.)
            char cached_path[512];
            if (!image_resolve_and_cache_to(raw_path, NULL, cached_path, sizeof(cached_path))) {
                return 1;
            }

            if (!image_is_supported(cached_path)) {
                return 1;
            }

            // Calculate display dimensions
            int img_cols = 0, img_rows_spec = 0;

            if (img_w < 0) img_cols = wrap_width * (-img_w) / 100;
            else if (img_w > 0) img_cols = img_w;
            if (img_cols > wrap_width) img_cols = wrap_width;
            if (img_cols <= 0) img_cols = wrap_width / 2;

            if (img_h < 0) img_rows_spec = text_height * (-img_h) / 100;
            else if (img_h > 0) img_rows_spec = img_h;

            int pixel_w, pixel_h;
            if (image_get_size(cached_path, &pixel_w, &pixel_h)) {
                int rows = image_calc_rows(pixel_w, pixel_h, img_cols, img_rows_spec);
                // Cache for later
                ((Block *)block)->data.image.display_rows = rows > 0 ? rows : 1;
                return rows > 0 ? rows : 1;
            }
            return 1;
        }

        case BLOCK_HEADER: {
            // Headers may use text scaling
            int level = block->data.header.level;
            int scale = 1;
            if (level == 1) scale = 2;
            else if (level == 2) scale = 1;  // 1.5x rounds to 2 rows for 1 line

            // Count content width and calculate wrapped lines
            size_t content_start = block->data.header.content_start;
            size_t end = block->end;
            if (end > 0 && gap_at(gb, end - 1) == '\n') end--;

            int total_width = 0;
            for (size_t p = content_start; p < end; ) {
                size_t next;
                total_width += gap_grapheme_width(gb, p, &next);
                p = next;
            }

            int available = wrap_width / scale;
            if (available < 1) available = 1;

            int lines = (total_width + available - 1) / available;
            if (lines < 1) lines = 1;

            return lines * scale;
        }

        case BLOCK_CODE: {
            // Count newlines in code content
            int lines = 1;
            for (size_t p = block->data.code.content_start;
                 p < block->data.code.content_start + block->data.code.content_len; p++) {
                if (gap_at(gb, p) == '\n') lines++;
            }
            return lines;
        }

        case BLOCK_MATH: {
            // Use cached tex_sketch if available
            TexSketch *sketch = (TexSketch *)block->data.math.tex_sketch;
            if (sketch) {
                return sketch->height > 0 ? sketch->height : 1;
            }

            // Render TeX to determine height and cache it
            size_t clen = block->data.math.content_len;
            size_t cstart = block->data.math.content_start;
            char *latex = malloc(clen + 1);
            if (latex) {
                for (size_t i = 0; i < clen; i++) {
                    latex[i] = gap_at(gb, cstart + i);
                }
                latex[clen] = '\0';
                sketch = tex_render_string(latex, clen, true);
                free(latex);
                if (sketch) {
                    // Cache for later use during rendering
                    ((Block *)block)->data.math.tex_sketch = sketch;
                    return sketch->height > 0 ? sketch->height : 1;
                }
            }
            return 1;
        }

        case BLOCK_TABLE: {
            // Calculate actual table vrows matching render_table_element logic
            // Need to parse table structure and calculate wrapped cell heights
            int vrows = 0;

            // Calculate column widths (same as render)
            int col_widths[MD_TABLE_MAX_COLS];
            int total_col_width = wrap_width - (block->data.table.col_count + 1);  // Account for borders
            int base_width = total_col_width / block->data.table.col_count;
            for (int ci = 0; ci < block->data.table.col_count; ci++) {
                col_widths[ci] = base_width > 0 ? base_width : 1;
            }

            // Parse all rows like render does
            size_t row_starts[64], row_lens[64];
            int row_count = 0;
            size_t scan_pos = block->start;
            size_t block_end = block->start + (size_t)(block->end - block->start);

            while (scan_pos < block_end && row_count < 64) {
                int scan_cols = 0;
                size_t scan_len = 0;

                if (row_count == 1) {
                    // Delimiter row
                    MdAlign dummy_align[MD_TABLE_MAX_COLS];
                    if (md_check_table_delimiter(gb, scan_pos, &scan_cols, dummy_align, &scan_len)) {
                        row_starts[row_count] = scan_pos;
                        row_lens[row_count] = scan_len;
                        row_count++;
                        scan_pos += scan_len;
                        continue;
                    }
                }

                if (md_check_table_header(gb, scan_pos, &scan_cols, &scan_len)) {
                    row_starts[row_count] = scan_pos;
                    row_lens[row_count] = scan_len;
                    row_count++;
                    scan_pos += scan_len;
                } else {
                    break;
                }
            }

            // Top border
            vrows++;

            // Calculate row heights and dividers
            for (int ri = 0; ri < row_count; ri++) {
                if (ri == 1) {
                    // Delimiter row
                    vrows++;
                } else {
                    // Parse cells and calculate max wrapped height
                    size_t cell_starts[MD_TABLE_MAX_COLS], cell_lens[MD_TABLE_MAX_COLS];
                    int cells = md_parse_table_row(gb, row_starts[ri], row_lens[ri],
                                                   cell_starts, cell_lens, MD_TABLE_MAX_COLS);
                    int max_lines = 1;
                    for (int ci = 0; ci < cells && ci < block->data.table.col_count; ci++) {
                        // Calculate wrapped lines for cell
                        int lines = 1, line_width = 0;
                        size_t p = cell_starts[ci], end = cell_starts[ci] + cell_lens[ci];
                        while (p < end) {
                            size_t dlen = 0;
                            MdStyle delim = md_check_delim(gb, p, &dlen);
                            if (delim != 0 && dlen > 0) { p += dlen; continue; }
                            size_t next;
                            int gw = gap_grapheme_width(gb, p, &next);
                            if (line_width + gw > col_widths[ci] && line_width > 0) {
                                lines++; line_width = gw;
                            } else {
                                line_width += gw;
                            }
                            p = next;
                        }
                        if (lines > max_lines) max_lines = lines;
                    }
                    vrows += max_lines;

                    // Row divider between data rows (not after header, not after last row)
                    if (ri < row_count - 1 && ri != 0) {
                        vrows++;
                    }
                }
            }

            // Bottom border
            vrows++;

            return vrows;
        }

        case BLOCK_BLOCKQUOTE:
        case BLOCK_LIST_ITEM:
        case BLOCK_FOOTNOTE_DEF:
        case BLOCK_PARAGRAPH:
        default: {
            // Count wrapped lines
            int vrows = 0;
            size_t pos = block->start;
            size_t end = block->end;

            while (pos < end) {
                // Find end of logical line
                size_t line_end = pos;
                while (line_end < end && gap_at(gb, line_end) != '\n') {
                    line_end++;
                }

                // Calculate wrapped lines for this logical line
                int line_width = 0;
                int line_vrows = 1;
                for (size_t p = pos; p < line_end; ) {
                    size_t next;
                    int gw = gap_grapheme_width(gb, p, &next);
                    if (line_width + gw > wrap_width && line_width > 0) {
                        line_vrows++;
                        line_width = gw;
                    } else {
                        line_width += gw;
                    }
                    p = next;
                }

                vrows += line_vrows;

                // Move past newline
                pos = line_end;
                if (pos < end && gap_at(gb, pos) == '\n') {
                    pos++;
                }
            }

            return vrows > 0 ? vrows : 1;
        }
    }
}

// #endregion

// #region Query Functions

Block *block_at_pos(BlockCache *bc, size_t byte_pos) {
    int idx = block_index_at_pos(bc, byte_pos);
    return idx >= 0 ? &bc->blocks[idx] : NULL;
}

Block *block_at_vrow(BlockCache *bc, int vrow) {
    if (!bc->valid || bc->count == 0) return NULL;

    // Binary search for block containing vrow
    int lo = 0, hi = bc->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        Block *b = &bc->blocks[mid];
        if (vrow < b->vrow_start) {
            hi = mid;
        } else if (vrow >= b->vrow_start + b->vrow_count) {
            lo = mid + 1;
        } else {
            return b;
        }
    }

    // If not found exactly, return the last block before this vrow
    if (lo > 0 && lo <= bc->count) {
        return &bc->blocks[lo - 1];
    }

    return bc->count > 0 ? &bc->blocks[0] : NULL;
}

int block_index_at_pos(BlockCache *bc, size_t byte_pos) {
    if (!bc->valid || bc->count == 0) return -1;

    // Binary search for block containing byte_pos
    int lo = 0, hi = bc->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        Block *b = &bc->blocks[mid];
        if (byte_pos < b->start) {
            hi = mid;
        } else if (byte_pos >= b->end) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }

    // Position is at end of document
    if (lo > 0 && byte_pos >= bc->blocks[lo - 1].end) {
        return lo - 1;
    }

    return lo < bc->count ? lo : bc->count - 1;
}

int calc_cursor_vrow_in_block(const Block *block, const GapBuffer *gb,
                              size_t cursor, int wrap_width) {
    if (cursor < block->start || cursor > block->end) {
        return 0;
    }

    if (wrap_width <= 0) wrap_width = 80;

    // For simple blocks, cursor is at vrow 0
    switch (block->type) {
        case BLOCK_HR:
            return 0;

        case BLOCK_IMAGE: {
            // When cursor is in image, it renders as raw wrapped text
            // Count newlines and wrapping from block start to cursor
            int vrow = 0;
            int col = 0;
            for (size_t p = block->start; p < cursor && p < block->end; ) {
                char c = gap_at(gb, p);
                if (c == '\n') {
                    vrow++;
                    col = 0;
                    p++;
                } else {
                    size_t next;
                    int gw = gap_grapheme_width(gb, p, &next);
                    if (col + gw > wrap_width && col > 0) {
                        vrow++;
                        col = gw;
                    } else {
                        col += gw;
                    }
                    p = next;
                }
            }
            return vrow;
        }

        case BLOCK_HEADER: {
            // Calculate cursor position in scaled header
            int level = block->data.header.level;
            int scale = (level == 1) ? 2 : 1;
            int available = wrap_width / scale;
            if (available < 1) available = 1;

            int char_col = 0;
            int row = 0;
            for (size_t p = block->start; p < cursor && p < block->end; ) {
                if (gap_at(gb, p) == '\n') break;
                size_t next;
                int gw = gap_grapheme_width(gb, p, &next);
                char_col += gw;
                if (char_col > available) {
                    row++;
                    char_col = gw;
                }
                p = next;
            }
            return row * scale;
        }

        case BLOCK_CODE:
        case BLOCK_MATH:
        case BLOCK_TABLE: {
            // Count newlines from block start to cursor
            int vrow = 0;
            for (size_t p = block->start; p < cursor && p < block->end; p++) {
                if (gap_at(gb, p) == '\n') vrow++;
            }
            return vrow;
        }

        default: {
            // General wrapping calculation
            int vrow = 0;
            size_t pos = block->start;

            while (pos < cursor && pos < block->end) {
                // Find end of logical line
                size_t line_end = pos;
                while (line_end < block->end && gap_at(gb, line_end) != '\n') {
                    line_end++;
                }

                // Calculate wrapped position within line
                int line_width = 0;
                for (size_t p = pos; p < cursor && p < line_end; ) {
                    size_t next;
                    int gw = gap_grapheme_width(gb, p, &next);
                    if (line_width + gw > wrap_width && line_width > 0) {
                        vrow++;
                        line_width = gw;
                    } else {
                        line_width += gw;
                    }
                    p = next;
                }

                // If cursor is within this line, we're done
                if (cursor <= line_end) break;

                // Count the remaining wrapped portions of this line
                for (size_t p = (cursor > pos ? cursor : pos); p < line_end; ) {
                    size_t next;
                    int gw = gap_grapheme_width(gb, p, &next);
                    if (line_width + gw > wrap_width && line_width > 0) {
                        vrow++;
                        line_width = gw;
                    } else {
                        line_width += gw;
                    }
                    p = next;
                }

                vrow++;  // For the newline
                pos = line_end + 1;
            }

            return vrow;
        }
    }
}

// #endregion

// #region Inline Run Parsing

//! Initial capacity for inline runs array
#define INLINE_RUN_INITIAL_CAPACITY 16

//! Add an inline run to a paragraph block
static InlineRun *paragraph_add_run(Block *block) {
    if (block->type != BLOCK_PARAGRAPH) return NULL;

    if (block->data.paragraph.run_count >= block->data.paragraph.run_capacity) {
        int new_cap = block->data.paragraph.run_capacity == 0
            ? INLINE_RUN_INITIAL_CAPACITY
            : block->data.paragraph.run_capacity * 2;
        InlineRun *new_runs = realloc(block->data.paragraph.runs,
                                      sizeof(InlineRun) * (size_t)new_cap);
        if (!new_runs) return NULL;
        block->data.paragraph.runs = new_runs;
        block->data.paragraph.run_capacity = new_cap;
    }

    InlineRun *run = &block->data.paragraph.runs[block->data.paragraph.run_count++];
    memset(run, 0, sizeof(InlineRun));
    return run;
}

void block_parse_inline_runs(Block *block, const GapBuffer *gb) {
    if (block->type != BLOCK_PARAGRAPH) return;

    // Free existing runs
    block_free_inline_runs(block);

    size_t pos = block->start;
    size_t end = block->end;

    // Style stack for tracking nested formatting
    struct {
        MdStyle style;
        size_t dlen;
    } style_stack[8];
    int style_depth = 0;
    MdStyle active_style = 0;

    // Current run state
    size_t run_start = pos;
    MdStyle run_style = 0;

    while (pos < end) {
        char c = gap_at(gb, pos);

        // Check for newline (ends current run but continues paragraph)
        if (c == '\n') {
            if (pos > run_start) {
                InlineRun *run = paragraph_add_run(block);
                if (run) {
                    run->byte_start = run_start;
                    run->byte_end = pos;
                    run->style = run_style;
                    run->type = RUN_TEXT;
                }
            }
            pos++;
            run_start = pos;
            run_style = active_style;
            continue;
        }

        // Check for link [text](url)
        size_t link_text_start, link_text_len, link_url_start, link_url_len, link_total;
        if (md_check_link(gb, pos, &link_text_start, &link_text_len,
                          &link_url_start, &link_url_len, &link_total)) {
            // End current text run
            if (pos > run_start) {
                InlineRun *run = paragraph_add_run(block);
                if (run) {
                    run->byte_start = run_start;
                    run->byte_end = pos;
                    run->style = run_style;
                    run->type = RUN_TEXT;
                }
            }

            // Add link run
            InlineRun *link_run = paragraph_add_run(block);
            if (link_run) {
                link_run->byte_start = pos;
                link_run->byte_end = pos + link_total;
                link_run->style = active_style;
                link_run->type = RUN_LINK;
                link_run->data.link.url_start = link_url_start;
                link_run->data.link.url_len = link_url_len;
            }

            pos += link_total;
            run_start = pos;
            run_style = active_style;
            continue;
        }

        // Check for footnote reference [^id]
        size_t fn_id_start, fn_id_len, fn_total;
        if (md_check_footnote_ref(gb, pos, &fn_id_start, &fn_id_len, &fn_total)) {
            // End current text run
            if (pos > run_start) {
                InlineRun *run = paragraph_add_run(block);
                if (run) {
                    run->byte_start = run_start;
                    run->byte_end = pos;
                    run->style = run_style;
                    run->type = RUN_TEXT;
                }
            }

            // Add footnote run
            InlineRun *fn_run = paragraph_add_run(block);
            if (fn_run) {
                fn_run->byte_start = pos;
                fn_run->byte_end = pos + fn_total;
                fn_run->style = active_style;
                fn_run->type = RUN_FOOTNOTE_REF;
                fn_run->data.footnote.id_start = fn_id_start;
                fn_run->data.footnote.id_len = fn_id_len;
            }

            pos += fn_total;
            run_start = pos;
            run_style = active_style;
            continue;
        }

        // Check for inline math $...$
        size_t math_content_start, math_content_len, math_total;
        if (md_check_inline_math(gb, pos, &math_content_start, &math_content_len, &math_total)) {
            // End current text run
            if (pos > run_start) {
                InlineRun *run = paragraph_add_run(block);
                if (run) {
                    run->byte_start = run_start;
                    run->byte_end = pos;
                    run->style = run_style;
                    run->type = RUN_TEXT;
                }
            }

            // Add math run
            InlineRun *math_run = paragraph_add_run(block);
            if (math_run) {
                math_run->byte_start = pos;
                math_run->byte_end = pos + math_total;
                math_run->style = active_style;
                math_run->type = RUN_INLINE_MATH;
                math_run->data.math.content_start = math_content_start;
                math_run->data.math.content_len = math_content_len;
            }

            pos += math_total;
            run_start = pos;
            run_style = active_style;
            continue;
        }

        // Check for emoji :shortcode:
        size_t emoji_sc_start, emoji_sc_len, emoji_total;
        const char *emoji = md_check_emoji(gb, pos, &emoji_sc_start, &emoji_sc_len, &emoji_total);
        if (emoji) {
            // End current text run
            if (pos > run_start) {
                InlineRun *run = paragraph_add_run(block);
                if (run) {
                    run->byte_start = run_start;
                    run->byte_end = pos;
                    run->style = run_style;
                    run->type = RUN_TEXT;
                }
            }

            // Add emoji run
            InlineRun *emoji_run = paragraph_add_run(block);
            if (emoji_run) {
                emoji_run->byte_start = pos;
                emoji_run->byte_end = pos + emoji_total;
                emoji_run->style = active_style;
                emoji_run->type = RUN_EMOJI;
                emoji_run->data.emoji.emoji = emoji;
            }

            pos += emoji_total;
            run_start = pos;
            run_style = active_style;
            continue;
        }

        // Check for style delimiter (*, **, `, ~~, ==, etc.)
        size_t dlen;
        MdStyle delim = md_check_delim(gb, pos, &dlen);
        if (delim && dlen > 0) {
            // Check if this closes an existing style
            bool closed = false;
            for (int i = style_depth - 1; i >= 0; i--) {
                if (style_stack[i].style == delim && style_stack[i].dlen == dlen) {
                    // End current text run with current style
                    if (pos > run_start) {
                        InlineRun *run = paragraph_add_run(block);
                        if (run) {
                            run->byte_start = run_start;
                            run->byte_end = pos;
                            run->style = run_style;
                            run->type = RUN_TEXT;
                        }
                    }

                    // Pop style
                    active_style &= ~delim;
                    style_depth = i;

                    pos += dlen;
                    run_start = pos;
                    run_style = active_style;
                    closed = true;
                    break;
                }
            }

            if (!closed && style_depth < 8) {
                // End current text run
                if (pos > run_start) {
                    InlineRun *run = paragraph_add_run(block);
                    if (run) {
                        run->byte_start = run_start;
                        run->byte_end = pos;
                        run->style = run_style;
                        run->type = RUN_TEXT;
                    }
                }

                // Push new style
                style_stack[style_depth].style = delim;
                style_stack[style_depth].dlen = dlen;
                style_depth++;
                active_style |= delim;

                pos += dlen;
                run_start = pos;
                run_style = active_style;
            }

            if (closed) continue;
            if (!closed && style_depth > 0) continue;
        }

        // Regular character - continue current run
        pos++;
    }

    // End final run if any content remains
    if (pos > run_start) {
        InlineRun *run = paragraph_add_run(block);
        if (run) {
            run->byte_start = run_start;
            run->byte_end = pos;
            run->style = run_style;
            run->type = RUN_TEXT;
        }
    }
}

void block_free_inline_runs(Block *block) {
    if (block->type != BLOCK_PARAGRAPH) return;

    free(block->data.paragraph.runs);
    block->data.paragraph.runs = NULL;
    block->data.paragraph.run_count = 0;
    block->data.paragraph.run_capacity = 0;
}

// #endregion
