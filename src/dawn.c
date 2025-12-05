// dawn.c
//! Frontends call into this via dawn_app.h API
//
// KNOWN ISSUES:
// - Max undo: Undo stack has hard limit (MAX_UNDO) - oldest states silently drop
// - Large files: No streaming/chunked rendering for very large documents
// - Block cache: Invalidated on any edit - could be optimized for local changes
// - Timer overflow: Timer uses int64_t timestamps, no overflow handling
// - Footnote scan: Linear scan for footnote definitions - O(n) per check

#include "dawn_types.h"
#include "dawn_app.h"

// Debug assert - only compiles in debug builds
#ifdef NDEBUG
#define DAWN_ASSERT(cond, fmt, ...) ((void)0)
#else
#define DAWN_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        platform_shutdown(); \
        fprintf(stderr, "\r\n\033[1;31mASSERT FAILED:\033[0m %s\r\n", #cond); \
        fprintf(stderr, "  at %s:%d\r\n", __FILE__, __LINE__); \
        fprintf(stderr, "  " fmt "\r\n" __VA_OPT__(,) __VA_ARGS__); \
        fflush(stderr); \
        exit(1); \
    } \
} while(0)
#endif

#include "dawn_gap.h"
#include "dawn_theme.h"
#include "dawn_clipboard.h"
#include "dawn_timer.h"
#include "dawn_file.h"
#include "dawn_chat.h"
#include "dawn_nav.h"
#include "dawn_md.h"
#include "dawn_wrap.h"
#include "dawn_image.h"
#include "dawn_input.h"
#include "dawn_footnote.h"
#include "dawn_render.h"
#include "dawn_utils.h"
#include "dawn_highlight.h"
#include "dawn_tex.h"
#include "dawn_toc.h"
#include "dawn_search.h"
#include "dawn_block.h"

// Platform capability check macro
#define HAS_CAP(cap) platform_has(cap)

// #region Consolidated Macros and Types

//! Maximum nesting depth for inline markdown styles
#define MAX_STYLE_DEPTH 8

//! Layout calculation result
typedef struct {
    int text_area_cols;
    int ai_cols;
    int ai_start_col;
    int margin;
    int text_width;
    int top_margin;
    int text_height;
} Layout;

//! Render context passed to rendering functions
typedef struct {
    Layout L;
    int max_row;
    size_t len;
    int *cursor_virtual_row;
    int *cursor_col;
} RenderCtx;

//! Inline style stack entry for tracking nested markdown formatting
typedef struct {
    MdStyle style;
    size_t dlen;
    size_t close_pos;
} StyleStackEntry;

//! Render state for second pass
typedef struct {
    int virtual_row;
    int col_width;
    size_t pos;
    MdStyle line_style;
    bool in_block_math;
    StyleStackEntry style_stack[MAX_STYLE_DEPTH];
    int style_depth;
    MdStyle active_style;
    int cursor_virtual_row;
    int cursor_col;
} RenderState;

// #endregion

// #region Macros for Common Patterns

//! Check if cursor is in a range and syntax hiding is disabled
#define CURSOR_IN(start, end) cursor_in_range(app.cursor, (start), (end), app.hide_cursor_syntax)

//! Check if editing is allowed (not in focus mode or preview mode)
#define CAN_EDIT() (!app.focus_mode && !app.preview_mode)

//! Check if any editing is allowed (not in preview mode)
#define CAN_MODIFY() (!app.preview_mode)

// #endregion

// #region Pure Helper Functions

//! Calculate layout dimensions based on window size and AI panel state
static inline Layout calc_layout(void) {
    Layout l = {0};
    l.text_area_cols = app.cols;
    l.ai_cols = 0;
    l.ai_start_col = app.cols + 1;

    if (app.ai_open) {
        l.ai_cols = app.cols * AI_PANEL_WIDTH / 100;
        if (l.ai_cols < 30) l.ai_cols = 30;
        if (l.ai_cols > app.cols - 40) l.ai_cols = app.cols - 40;
        l.text_area_cols = app.cols - l.ai_cols - 1;
        l.ai_start_col = l.text_area_cols + 1;
    }

    l.margin = l.text_area_cols > 80 ? (l.text_area_cols - 70) / 2 : 4;
    l.text_width = l.text_area_cols - l.margin * 2;
    l.top_margin = 2;
    l.text_height = app.rows - l.top_margin - 2;
    return l;
}

//! Calculate screen row from virtual row
static inline int vrow_to_screen(const Layout *L, int vrow, int scroll_y) {
    return L->top_margin + (vrow - scroll_y);
}

//! Check if screen row is visible
static inline bool is_row_visible(const Layout *L, int screen_row, int max_row) {
    return screen_row >= L->top_margin && screen_row <= max_row;
}

//! Check if cursor is within a range, respecting hide_cursor_syntax toggle
static inline bool cursor_in_range(size_t cursor, size_t start, size_t end, bool hide_syntax) {
    return (cursor >= start && cursor < end) && !hide_syntax;
}

//! Track cursor position during rendering
static inline void track_cursor(size_t pos, int vrow, int col, int margin,
                                int *out_cursor_vrow, int *out_cursor_col) {
    if (pos == app.cursor) {
        *out_cursor_vrow = vrow;
        *out_cursor_col = margin + 1 + col;
    }
}


static inline int get_line_scale(MdStyle line_style) {
    return HAS_CAP(PLATFORM_CAP_TEXT_SIZING) ? md_get_scale(line_style) : 1;
}

//! Skip leading whitespace for wrapped lines
static inline size_t skip_leading_space(const GapBuffer *gb, size_t pos, size_t end) {
    while (pos < end) {
        size_t char_len;
        utf8proc_int32_t cp = gap_utf8_at(gb, pos, &char_len);
        if (cp != ' ') break;
        pos += char_len;
    }
    return pos;
}

//! Delete selection if present, updating cursor
static inline void delete_selection_if_any(void) {
    if (has_selection()) {
        size_t s, e;
        get_selection(&s, &e);
        gap_delete(&app.text, s, e - s);
        app.cursor = s;
        app.selecting = false;
    }
}

//! Find start of current line from cursor position
static inline size_t find_line_start(size_t cursor) {
    size_t result = cursor;
    while (result > 0 && gap_at(&app.text, result - 1) != '\n') result--;
    return result;
}

//! Copy text from gap buffer into char array
static inline void gap_copy_str(const GapBuffer *gb, size_t start, size_t len, char *out, size_t out_size) {
    size_t copy_len = len < out_size - 1 ? len : out_size - 1;
    for (size_t i = 0; i < copy_len; i++) out[i] = gap_at(gb, start + i);
    out[copy_len] = '\0';
}

//! Check if a list/blockquote item is empty
static inline bool is_item_content_empty(const GapBuffer *gb, size_t cursor, size_t content_start) {
    if (cursor == content_start) return true;
    if (content_start < gap_len(gb) && gap_at(gb, content_start) == '\n') return true;
    return false;
}

//! Insert a string at cursor position and advance cursor
static inline void insert_str_at_cursor(GapBuffer *gb, size_t *cursor, const char *str) {
    while (*str) {
        gap_insert(gb, *cursor, *str++);
        (*cursor)++;
    }
}

//! Insert N copies of a character at cursor position
static inline void insert_chars_at_cursor(GapBuffer *gb, size_t *cursor, char c, int count) {
    for (int i = 0; i < count; i++) {
        gap_insert(gb, *cursor, c);
        (*cursor)++;
    }
}

//! Handle empty list/quote item - delete marker and insert newline
static inline void handle_empty_list_item(GapBuffer *gb, size_t *cursor, size_t line_start) {
    gap_delete(gb, line_start, *cursor - line_start);
    *cursor = line_start;
    gap_insert(gb, *cursor, '\n');
    (*cursor)++;
}

//! Get current text width for word wrapping
static inline int get_text_width(void) { return calc_layout().text_width; }

//! Recalculate wrap segment after prefix rendering
static inline void recalc_wrap_seg(int text_width, int col_width, size_t pos, size_t line_end,
                                   size_t *seg_end, int *seg_width) {
    int available = text_width - col_width;
    if (available < 1) available = 1;
    *seg_end = gap_find_wrap_point(&app.text, pos, line_end, available, seg_width);
}

// #endregion

// #region Markdown Element Helpers

//! Find a markdown element (image, link, footnote, inline math) containing the given position
static bool md_find_element_at(const GapBuffer *gb, size_t cursor, size_t *out_start, size_t *out_len) {
    size_t len = gap_len(gb);
    size_t scan_start = cursor > 100 ? cursor - 100 : 0;

    // Check image
    for (size_t p = scan_start; p <= cursor && p < len; p++) {
        size_t alt_s, alt_l, path_s, path_l, total;
        int img_w, img_h;
        if (md_check_image(gb, p, &alt_s, &alt_l, &path_s, &path_l, &img_w, &img_h, &total)) {
            if (cursor >= p && cursor < p + total) {
                *out_start = p; *out_len = total; return true;
            }
        }
    }
    // Check link
    for (size_t p = scan_start; p <= cursor && p < len; p++) {
        size_t text_s, text_l, url_s, url_l, total;
        if (md_check_link(gb, p, &text_s, &text_l, &url_s, &url_l, &total)) {
            if (cursor >= p && cursor < p + total) {
                *out_start = p; *out_len = total; return true;
            }
        }
    }
    // Check footnote ref
    for (size_t p = scan_start; p <= cursor && p < len; p++) {
        size_t id_s, id_l, total;
        if (md_check_footnote_ref(gb, p, &id_s, &id_l, &total)) {
            if (cursor >= p && cursor < p + total) {
                *out_start = p; *out_len = total; return true;
            }
        }
    }
    // Check inline math
    for (size_t p = scan_start; p <= cursor && p < len; p++) {
        size_t c_s, c_l, total;
        if (md_check_inline_math(gb, p, &c_s, &c_l, &total)) {
            if (cursor >= p && cursor < p + total) {
                *out_start = p; *out_len = total; return true;
            }
        }
    }
    return false;
}

//! Check if markdown style at pos has a matching closing delimiter
static size_t md_find_closing(const GapBuffer *gb, size_t pos, MdStyle style, size_t dlen) {
    size_t len = gap_len(gb);
    size_t p = pos + dlen;

    while (p < len) {
        char c = gap_at(gb, p);
        if (c == '\n') return 0;

        size_t check_dlen = 0;
        MdStyle check_style = md_check_delim(gb, p, &check_dlen);

        if (check_style == style && check_dlen == dlen) {
            return p;
        }

        if (check_dlen > 0) {
            p += check_dlen;
        } else {
            p++;
        }
    }
    return 0;
}

//! Check if a footnote definition with given ID exists in the text
static bool footnote_def_exists(const GapBuffer *gb, const char *id) {
    size_t len = gap_len(gb);
    for (size_t pos = 0; pos < len; pos++) {
        size_t def_id_start, def_id_len, content_start, def_total;
        if (md_check_footnote_def(gb, pos, &def_id_start, &def_id_len, &content_start, &def_total)) {
            char def_id[64];
            gap_copy_str(gb, def_id_start, def_id_len, def_id, sizeof(def_id));
            if (strcmp(id, def_id) == 0) return true;
        }
    }
    return false;
}

//! Auto-create footnote definition when user types [^id]
static void maybe_create_footnote_def(GapBuffer *gb, size_t cursor) {
    if (cursor < 4) return;

    for (size_t back = 3; back < 64 && back < cursor; back++) {
        size_t check_pos = cursor - back - 1;
        size_t id_start, id_len, total;
        if (!md_check_footnote_ref(gb, check_pos, &id_start, &id_len, &total)) continue;

        char id[64];
        gap_copy_str(gb, id_start, id_len, id, sizeof(id));
        if (footnote_def_exists(gb, id)) return;

        size_t len = gap_len(gb);
        size_t insert_pos = len;

        bool first = true;
        for (size_t p = 0; p < len; p++) {
            size_t d1, d2, d3, d4;
            if (md_check_footnote_def(gb, p, &d1, &d2, &d3, &d4)) { first = false; break; }
        }

        if (len > 0 && gap_at(gb, len - 1) != '\n') gap_insert(gb, insert_pos++, '\n');
        gap_insert(gb, insert_pos++, '\n');

        if (first) {
            const char *sep = "---\n\n";
            while (*sep) gap_insert(gb, insert_pos++, *sep++);
        }

        gap_insert(gb, insert_pos++, '[');
        gap_insert(gb, insert_pos++, '^');
        for (const char *p = id; *p; p++) gap_insert(gb, insert_pos++, *p);
        gap_insert(gb, insert_pos++, ']');
        gap_insert(gb, insert_pos++, ':');
        gap_insert(gb, insert_pos++, ' ');
        return;
    }
}

// #endregion

// #region Image Helpers

//! Helper to resolve image path and calculate display rows
static int calc_image_rows_for_md(const GapBuffer *gb, size_t path_start, size_t path_len,
                                   int img_w, int img_h, int text_width, int text_height,
                                   char *resolved_out, size_t resolved_size) {
    char raw_path[512];
    size_t plen = path_len < sizeof(raw_path) - 1 ? path_len : sizeof(raw_path) - 1;
    for (size_t i = 0; i < plen; i++) {
        raw_path[i] = gap_at(gb, path_start + i);
    }
    raw_path[plen] = '\0';

    char cached_path[512];
    if (!image_resolve_and_cache_to(raw_path, NULL, cached_path, sizeof(cached_path))) {
        return 0;
    }

    if (resolved_out && resolved_size > 0) {
        strncpy(resolved_out, cached_path, resolved_size - 1);
        resolved_out[resolved_size - 1] = '\0';
    }

    if (!image_is_supported(cached_path)) return 0;

    int img_cols = 0, img_rows_spec = 0;

    if (img_w < 0) img_cols = text_width * (-img_w) / 100;
    else if (img_w > 0) img_cols = img_w;
    if (img_cols > text_width) img_cols = text_width;
    if (img_cols <= 0) img_cols = text_width / 2;

    if (img_h < 0) img_rows_spec = text_height * (-img_h) / 100;
    else if (img_h > 0) img_rows_spec = img_h;

    int pixel_w, pixel_h;
    if (image_get_size(cached_path, &pixel_w, &pixel_h)) {
        return image_calc_rows(pixel_w, pixel_h, img_cols, img_rows_spec);
    }
    return 0;
}

// #endregion

// #region Global State

//! Global application state
App app = {0};

// #endregion

// #region Undo/Redo

//! Save current text state to undo stack
static void save_undo_state(void) {
    if (app.undo_pos < app.undo_count - 1) {
        for (int i = app.undo_pos + 1; i < app.undo_count; i++) {
            free(app.undo_stack[i].text);
        }
        app.undo_count = app.undo_pos + 1;
    }

    if (app.undo_count >= MAX_UNDO) {
        free(app.undo_stack[0].text);
        memmove(&app.undo_stack[0], &app.undo_stack[1], (MAX_UNDO - 1) * sizeof(app.undo_stack[0]));
        app.undo_count--;
        app.undo_pos--;
    }

    size_t text_len = gap_len(&app.text);
    char *saved_text = malloc(text_len);
    if (saved_text) {
        gap_copy_to(&app.text, 0, text_len, saved_text);
        app.undo_stack[app.undo_count].text = saved_text;
        app.undo_stack[app.undo_count].text_len = text_len;
        app.undo_stack[app.undo_count].cursor = app.cursor;
        app.undo_count++;
        app.undo_pos = app.undo_count - 1;
    }
}

//! Restore undo state at given position
static void restore_undo_state(int pos) {
    size_t current_len = gap_len(&app.text);
    if (current_len > 0) gap_delete(&app.text, 0, current_len);
    gap_insert_str(&app.text, 0, app.undo_stack[pos].text, app.undo_stack[pos].text_len);
    app.cursor = app.undo_stack[pos].cursor;
    size_t len = gap_len(&app.text);
    if (app.cursor > len) app.cursor = len;
}

static void undo(void) {
    if (app.undo_pos > 0) {
        app.undo_pos--;
        restore_undo_state(app.undo_pos);
    }
}

static void redo(void) {
    if (app.undo_pos < app.undo_count - 1) {
        app.undo_pos++;
        restore_undo_state(app.undo_pos);
    }
}

// #endregion

// #region Smart Editing Helpers

static bool check_smart_delete_symbol(size_t *del_start, size_t *del_len) {
    if (app.cursor < 3) return false;

    char c1 = gap_at(&app.text, app.cursor - 1);
    char c2 = gap_at(&app.text, app.cursor - 2);
    char c3 = gap_at(&app.text, app.cursor - 3);

    if (c1 == ')' && c2 == 'c' && c3 == '(') {
        *del_start = app.cursor - 3; *del_len = 3; return true;
    }
    if (c1 == ')' && c2 == 'r' && c3 == '(') {
        *del_start = app.cursor - 3; *del_len = 3; return true;
    }
    if (app.cursor >= 4) {
        char c4 = gap_at(&app.text, app.cursor - 4);
        if (c1 == ')' && c2 == 'm' && c3 == 't' && c4 == '(') {
            *del_start = app.cursor - 4; *del_len = 4; return true;
        }
    }
    return false;
}

//! Scan backwards for paired delimiter
static bool scan_for_paired_delim(char delim, size_t count, size_t *del_start, size_t *del_len) {
    size_t check_count = count;
    for (size_t i = app.cursor - count; i > 0 && i >= count; i--) {
        bool match = true;
        for (size_t j = 0; j < check_count; j++) {
            if (gap_at(&app.text, i - 1 - j) != delim) { match = false; break; }
        }
        if (match) {
            *del_start = i - check_count;
            *del_len = app.cursor - *del_start;
            return true;
        }
    }
    return false;
}

static bool check_smart_delete_delimiter(size_t *del_start, size_t *del_len) {
    size_t len = gap_len(&app.text);
    if (app.cursor == 0) return false;

    char c = gap_at(&app.text, app.cursor - 1);

    // ** (bold) or * (italic)
    if (c == '*') {
        if (app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '*') {
            if (scan_for_paired_delim('*', 2, del_start, del_len)) return true;
        } else {
            for (size_t i = app.cursor - 1; i > 0; i--) {
                char prev = gap_at(&app.text, i - 1);
                if (prev == '*') {
                    bool is_double = false;
                    if (i >= 2 && gap_at(&app.text, i - 2) == '*') is_double = true;
                    if (i < len && gap_at(&app.text, i) == '*') is_double = true;
                    if (!is_double) {
                        *del_start = i - 1;
                        *del_len = app.cursor - (i - 1);
                        return true;
                    }
                }
            }
        }
    }

    // ~~ (strikethrough)
    if (c == '~' && app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '~') {
        if (scan_for_paired_delim('~', 2, del_start, del_len)) return true;
    }

    // == (highlight)
    if (c == '=' && app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '=') {
        if (scan_for_paired_delim('=', 2, del_start, del_len)) return true;
    }

    // $ (inline math)
    if (c == '$') {
        for (size_t i = app.cursor - 1; i > 0; i--) {
            if (gap_at(&app.text, i - 1) == '$') {
                *del_start = i - 1;
                *del_len = app.cursor - (i - 1);
                return true;
            }
        }
    }
    return false;
}

static bool check_smart_delete_structure(size_t *del_start, size_t *del_len) {
    if (app.cursor == 0) return false;
    char c = gap_at(&app.text, app.cursor - 1);

    if (c == ')' || c == '}') {
        for (size_t i = app.cursor; i > 0 && (app.cursor - i) < 500; i--) {
            size_t alt_start, alt_len, url_start, url_len, total_len;
            int img_width, img_height;
            if (md_check_image(&app.text, i - 1, &alt_start, &alt_len, &url_start, &url_len,
                              &img_width, &img_height, &total_len)) {
                if (i - 1 + total_len == app.cursor) {
                    *del_start = i - 1; *del_len = total_len; return true;
                }
            }
            size_t text_start, text_len, link_url_start, link_url_len, link_total_len;
            if (md_check_link(&app.text, i - 1, &text_start, &text_len, &link_url_start,
                             &link_url_len, &link_total_len)) {
                if (i - 1 + link_total_len == app.cursor) {
                    *del_start = i - 1; *del_len = link_total_len; return true;
                }
            }
        }
    }

    if (c == ']') {
        for (size_t i = app.cursor; i > 0 && (app.cursor - i) < 100; i--) {
            size_t id_start, id_len, total;
            if (md_check_footnote_ref(&app.text, i - 1, &id_start, &id_len, &total)) {
                if (i - 1 + total == app.cursor) {
                    *del_start = i - 1; *del_len = total; return true;
                }
            }
        }
    }
    return false;
}

static bool smart_backspace(void) {
    size_t del_start, del_len;
    if (check_smart_delete_symbol(&del_start, &del_len) ||
        check_smart_delete_structure(&del_start, &del_len) ||
        check_smart_delete_delimiter(&del_start, &del_len)) {
        gap_delete(&app.text, del_start, del_len);
        app.cursor = del_start;
        return true;
    }
    return false;
}

static void check_auto_newline(char typed_char) {
    size_t len = gap_len(&app.text);

    if (typed_char == '-' && app.cursor >= 3) {
        size_t line_start = find_line_start(app.cursor);
        size_t rule_len;
        if (md_check_hr(&app.text, line_start, &rule_len)) {
            if (line_start + rule_len == app.cursor) {
                gap_insert(&app.text, app.cursor, '\n');
                app.cursor++;
                return;
            }
        }
    }

    if (typed_char == ')' || typed_char == '}') {
        for (size_t i = app.cursor; i > 0 && (app.cursor - i) < 500; i--) {
            size_t alt_start, alt_len, url_start, url_len, total_len;
            int img_width, img_height;
            if (md_check_image(&app.text, i - 1, &alt_start, &alt_len, &url_start, &url_len,
                              &img_width, &img_height, &total_len)) {
                if (i - 1 + total_len == app.cursor) {
                    gap_insert(&app.text, app.cursor, '\n');
                    app.cursor++;
                    return;
                }
            }
        }
    }

    if (typed_char == '$' && app.cursor >= 4) {
        if (app.cursor >= 2 &&
            gap_at(&app.text, app.cursor - 1) == '$' &&
            gap_at(&app.text, app.cursor - 2) == '$') {
            for (size_t i = app.cursor - 2; i >= 2; i--) {
                if (gap_at(&app.text, i - 1) == '$' && gap_at(&app.text, i - 2) == '$') {
                    gap_insert(&app.text, app.cursor, '\n');
                    app.cursor++;
                    return;
                }
                if (i == 0) break;
            }
        }
    }

    if (typed_char == '`' && app.cursor >= 3) {
        if (gap_at(&app.text, app.cursor - 1) == '`' &&
            gap_at(&app.text, app.cursor - 2) == '`' &&
            gap_at(&app.text, app.cursor - 3) == '`') {
            size_t line_start = find_line_start(app.cursor);
            if (line_start + 3 == app.cursor) {
                bool found_opening = false;
                size_t pos = line_start;
                while (pos >= 2) {
                    pos--;
                    if (gap_at(&app.text, pos) == '\n' || pos == 0) {
                        size_t check_pos = (gap_at(&app.text, pos) == '\n') ? pos + 1 : pos;
                        if (check_pos + 3 <= len &&
                            gap_at(&app.text, check_pos) == '`' &&
                            gap_at(&app.text, check_pos + 1) == '`' &&
                            gap_at(&app.text, check_pos + 2) == '`') {
                            found_opening = true;
                            break;
                        }
                    }
                    if (pos == 0) break;
                }
                if (found_opening) {
                    gap_insert(&app.text, app.cursor, '\n');
                    app.cursor++;
                    return;
                }
            }
        }
    }
}

// #endregion

// #region Chat Markdown Rendering

//! Print text with inline markdown formatting for AI chat
static void chat_print_md(const char *text, size_t start, int len) {
    bool in_bold = false, in_italic = false, in_code = false;
    bool in_link_text = false, in_link_url = false;

    for (int i = 0; i < len; i++) {
        size_t pos = start + (size_t)i;
        char c = text[pos];
        char next = (i + 1 < len) ? text[pos + 1] : '\0';

        if (c == '`' && !in_link_url) {
            in_code = !in_code;
            if (in_code) {
                set_dim(true);
            } else {
                reset_attrs(); set_fg(get_fg()); set_bg(get_ai_bg());
                if (in_bold) set_bold(true);
                if (in_italic) set_italic(true);
            }
            continue;
        }

        if (in_code) { out_char(c); continue; }

        if (c == '*' && next == '*' && !in_link_url) {
            in_bold = !in_bold;
            if (in_bold) set_bold(true);
            else { reset_attrs(); set_fg(get_fg()); set_bg(get_ai_bg()); if (in_italic) set_italic(true); }
            i++;
            continue;
        }

        if (c == '*' && !in_link_url) {
            in_italic = !in_italic;
            if (in_italic) set_italic(true);
            else { reset_attrs(); set_fg(get_fg()); set_bg(get_ai_bg()); if (in_bold) set_bold(true); }
            continue;
        }

        if (c == '[' && !in_link_text && !in_link_url) {
            in_link_text = true;
            set_fg(get_accent()); set_underline(UNDERLINE_STYLE_CURLY);
            continue;
        }
        if (c == ']' && in_link_text && next == '(') {
            in_link_text = false; in_link_url = true;
            reset_attrs(); set_fg(get_fg()); set_bg(get_ai_bg());
            if (in_bold) set_bold(true);
            if (in_italic) set_italic(true);
            i++;
            continue;
        }
        if (c == ')' && in_link_url) { in_link_url = false; continue; }
        if (in_link_url) continue;

        out_char(c);
    }
    reset_attrs(); set_fg(get_fg()); set_bg(get_ai_bg());
}

// #endregion

// #region Render Helpers - Grapheme Output

//! Output grapheme and advance position, returning display width
static inline int output_grapheme_advance(const GapBuffer *gb, size_t *pos) {
    return output_grapheme(gb, pos);
}

//! Get grapheme width and next position without output
static inline int grapheme_width_next(const GapBuffer *gb, size_t pos, size_t *next) {
    return gap_grapheme_width(gb, pos, next);
}

//! Wrap check and render a grapheme
static void wrap_and_render_grapheme(const RenderCtx *ctx, size_t *pos, int *col,
                                     int *vrow, int *srow) {
    size_t next;
    int gw = grapheme_width_next(&app.text, *pos, &next);
    if (*col + gw > ctx->L.text_width && *col > 0) {
        (*vrow)++;
        *col = 0;
        *srow = vrow_to_screen(&ctx->L, *vrow, app.scroll_y);
    }
    if (is_row_visible(&ctx->L, *srow, ctx->max_row)) {
        if (*col == 0) move_to(*srow, ctx->L.margin + 1);
        *col += output_grapheme_advance(&app.text, pos);
    } else {
        *col += gw;
        *pos = next;
    }
}

// #endregion

// Forward declarations
static void render_writing(void);

static void update_title(void) {
    const PlatformBackend *p = platform_get();
    if (!p || !p->set_title) return;

    switch (app.mode) {
        case MODE_WELCOME:
            p->set_title("Dawn");
            break;
        case MODE_HISTORY:
            p->set_title("Dawn | History");
            break;
        case MODE_TIMER_SELECT:
            p->set_title("Dawn | Timer");
            break;
        case MODE_HELP:
            p->set_title("Dawn | Help");
            break;
        case MODE_WRITING:
        case MODE_TITLE_EDIT:
        case MODE_IMAGE_EDIT:
        case MODE_TOC:
        case MODE_SEARCH:
            // Use document title if available, otherwise "Dawn"
            p->set_title(app.session_title ? app.session_title : "Dawn");
            break;
        default:
            p->set_title("Dawn");
            break;
    }
}

// #region Render Helpers - Raw Content

//! Render raw dimmed content (block element with newlines)
static void render_raw_dimmed_block(const RenderCtx *ctx, size_t *pos, int *col,
                                    int *vrow, int *srow, size_t end_pos) {
    set_fg(get_dim());
    while (*pos < end_pos && *pos < ctx->len) {
        *srow = vrow_to_screen(&ctx->L, *vrow, app.scroll_y);
        track_cursor(*pos, *vrow, *col, ctx->L.margin, ctx->cursor_virtual_row, ctx->cursor_col);
        char ch = gap_at(&app.text, *pos);
        if (ch == '\n') {
            (*pos)++; (*vrow)++; *col = 0;
            if (is_row_visible(&ctx->L, *srow, ctx->max_row))
                move_to(*srow + 1, ctx->L.margin + 1);
        } else if (ch == '\t') {
            int tab_width = 4 - (*col % 4);
            if (is_row_visible(&ctx->L, *srow, ctx->max_row)) {
                for (int ti = 0; ti < tab_width; ti++) {
                    out_char(' ');
                }
            }
            *col += tab_width;
            (*pos)++;
        } else {
            wrap_and_render_grapheme(ctx, pos, col, vrow, srow);
        }
    }
    set_fg(get_fg());
}

//! Render raw dimmed inline element (no newlines)
static void render_raw_dimmed_inline(const RenderCtx *ctx, size_t *pos, int *col,
                                     int *vrow, int *srow, size_t end_pos) {
    set_fg(get_dim());
    while (*pos < end_pos && *pos < ctx->len) {
        *srow = vrow_to_screen(&ctx->L, *vrow, app.scroll_y);
        track_cursor(*pos, *vrow, *col, ctx->L.margin, ctx->cursor_virtual_row, ctx->cursor_col);
        wrap_and_render_grapheme(ctx, pos, col, vrow, srow);
    }
    set_fg(get_fg());
}

//! Render raw prefix with cursor tracking
static void render_raw_prefix(size_t *pos, size_t content_end, int *col, size_t len,
                              int *cursor_vrow, int *cursor_col, int vrow, int margin) {
    set_fg(get_dim());
    while (*pos < content_end && *pos < len) {
        track_cursor(*pos, vrow, *col, margin, cursor_vrow, cursor_col);
        *col += output_grapheme_advance(&app.text, pos);
    }
    set_fg(get_fg());
}

// #endregion

// #region Table Rendering Helpers

//! Calculate wrapped lines for a table cell
static int calc_cell_wrapped_lines(size_t cell_start, size_t cell_len, int col_width) {
    int lines = 1, line_width = 0;
    size_t p = cell_start, end = cell_start + cell_len;
    
    while (p < end) {
        size_t dlen = 0;
        MdStyle delim = md_check_delim(&app.text, p, &dlen);
        if (delim != 0 && dlen > 0) { p += dlen; continue; }
        
        size_t next;
        int gw = grapheme_width_next(&app.text, p, &next);
        if (line_width + gw > col_width && line_width > 0) {
            lines++; line_width = gw;
        } else {
            line_width += gw;
        }
        p = next;
    }
    return lines;
}

//! Calculate column widths for table
static void calc_table_col_widths(int col_count, int text_width, int *col_widths) {
    int border_overhead = (col_count * 3) + 1;
    int available_width = text_width - border_overhead;
    int base_col_width = available_width / col_count;
    if (base_col_width < 8) base_col_width = 8;
    if (base_col_width > 30) base_col_width = 30;
    
    for (int i = 0; i < col_count; i++) {
        col_widths[i] = base_col_width;
    }
}

//! Render table horizontal border
static void render_table_hborder(const Layout *L, int screen_row, int max_row,
                                 int col_count, const int *col_widths,
                                 const char *left, const char *mid, const char *right) {
    if (!is_row_visible(L, screen_row, max_row)) return;
    
    move_to(screen_row, L->margin + 1);
    set_fg(get_border());
    out_str(left);
    for (int ci = 0; ci < col_count; ci++) {
        for (int w = 0; w < col_widths[ci] + 2; w++) out_str("─");
        if (ci < col_count - 1) out_str(mid);
    }
    out_str(right);
    set_fg(get_fg());
}

// #endregion

// #region Scroll Calculation

//! Calculate virtual row for table at given position
static int calc_table_vrows(size_t pos, const MdTable *tbl, const Layout *L,
                           size_t cursor_pos, int *cursor_vrow) {
    int col_widths[MD_TABLE_MAX_COLS];
    calc_table_col_widths(tbl->col_count, L->text_width, col_widths);
    
    bool cursor_in_table = (cursor_pos >= pos && cursor_pos < pos + tbl->total_len);
    size_t scan_pos = pos;
    int source_row = 0;
    int vrow = 1;  // Top border
    
    while (scan_pos < pos + tbl->total_len) {
        int scan_cols = 0;
        size_t scan_len = 0;
        
        if (source_row == 1) {
            MdAlign dummy_align[MD_TABLE_MAX_COLS];
            if (md_check_table_delimiter(&app.text, scan_pos, &scan_cols, dummy_align, &scan_len)) {
                if (cursor_in_table && cursor_pos >= scan_pos && cursor_pos < scan_pos + scan_len) {
                    *cursor_vrow += vrow;
                }
                vrow++;
                scan_pos += scan_len;
                source_row++;
                continue;
            }
        }
        
        if (md_check_table_header(&app.text, scan_pos, &scan_cols, &scan_len)) {
            if (cursor_in_table && cursor_pos >= scan_pos && cursor_pos < scan_pos + scan_len) {
                *cursor_vrow += vrow;
            }
            
            size_t cell_starts[MD_TABLE_MAX_COLS], cell_lens[MD_TABLE_MAX_COLS];
            int cells = md_parse_table_row(&app.text, scan_pos, scan_len,
                                           cell_starts, cell_lens, MD_TABLE_MAX_COLS);
            
            int max_lines = 1;
            for (int ci = 0; ci < cells && ci < tbl->col_count; ci++) {
                int cell_lines = calc_cell_wrapped_lines(cell_starts[ci], cell_lens[ci], col_widths[ci]);
                if (cell_lines > max_lines) max_lines = cell_lines;
            }
            
            vrow += max_lines;
            
            if (source_row >= 2) {
                size_t peek_pos = scan_pos + scan_len;
                int peek_cols = 0;
                size_t peek_len = 0;
                if (peek_pos < pos + tbl->total_len &&
                    md_check_table_header(&app.text, peek_pos, &peek_cols, &peek_len)) {
                    vrow++;
                }
            }
            
            scan_pos += scan_len;
            source_row++;
        } else {
            break;
        }
    }
    
    return vrow + 1;  // +1 for bottom border
}

// #endregion

// #region Block Element Rendering

//! Render image element
static bool render_image_element(const RenderCtx *ctx, RenderState *rs,
                                 size_t img_alt_start, size_t img_alt_len,
                                 size_t img_path_start, size_t img_path_len,
                                 int img_w, int img_h, size_t img_total_len) {
    (void)img_alt_start; (void)img_alt_len;
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    if (cursor_in_range(app.cursor, rs->pos, rs->pos + img_total_len, app.hide_cursor_syntax)) {
        // Cursor is inside image - render raw dimmed using block renderer for proper newline handling
        render_raw_dimmed_block(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row,
                                rs->pos + img_total_len);
        return true;
    }

    // Track cursor position at image
    track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                &rs->cursor_virtual_row, &rs->cursor_col);

    char resolved_path[1024];
    resolved_path[0] = '\0';
    int img_rows = calc_image_rows_for_md(&app.text, img_path_start, img_path_len,
                                          img_w, img_h, ctx->L.text_width, ctx->L.text_height,
                                          resolved_path, sizeof(resolved_path));

    // Ensure at least 1 row for image placeholder if image can't be displayed
    if (img_rows <= 0) img_rows = 1;

    rs->pos += img_total_len;

    int img_screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
    int img_end_row = img_screen_row + img_rows;

    if (img_end_row > ctx->L.top_margin && img_screen_row < ctx->max_row && resolved_path[0]) {
        int img_cols = 0;
        if (img_w < 0) img_cols = ctx->L.text_width * (-img_w) / 100;
        else if (img_w > 0) img_cols = img_w;
        else img_cols = ctx->L.text_width / 2;
        if (img_cols > ctx->L.text_width) img_cols = ctx->L.text_width;

        int crop_top_rows = 0, visible_rows = img_rows;
        int draw_row = img_screen_row;

        if (img_screen_row < ctx->L.top_margin) {
            crop_top_rows = ctx->L.top_margin - img_screen_row;
            visible_rows -= crop_top_rows;
            draw_row = ctx->L.top_margin;
        }
        if (img_end_row > ctx->max_row) {
            visible_rows = ctx->max_row - draw_row;
        }

        if (visible_rows > 0) {
            move_to(draw_row, ctx->L.margin + 1);
            if (crop_top_rows > 0 || visible_rows < img_rows) {
                image_display_at_cropped(resolved_path, draw_row, ctx->L.margin + 1,
                                         img_cols, crop_top_rows, visible_rows);
            } else {
                image_display_at(resolved_path, draw_row, ctx->L.margin + 1, img_cols, 0);
            }
        }
    }
    rs->virtual_row += img_rows;
    rs->col_width = 0;
    return true;
}

//! Render HR element
static bool render_hr_element(const RenderCtx *ctx, RenderState *rs, size_t hr_len) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
    bool cursor_in_hr = cursor_in_range(app.cursor, rs->pos, rs->pos + hr_len, app.hide_cursor_syntax);
    
    if (cursor_in_hr) {
        set_fg(get_dim());
        for (size_t i = 0; i < hr_len && rs->pos < ctx->len; i++) {
            screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
            track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                        &rs->cursor_virtual_row, &rs->cursor_col);
            char ch = gap_at(&app.text, rs->pos);
            if (ch == '\n') { rs->pos++; break; }
            wrap_and_render_grapheme(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row);
        }
        set_fg(get_fg());
    } else {
        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
            move_to(screen_row, ctx->L.margin + 1);
            set_fg(get_dim());
            for (int i = 0; i < ctx->L.text_width; i++) out_str("─");
            set_fg(get_fg());
        }
        if (app.cursor >= rs->pos && app.cursor < rs->pos + hr_len) {
            rs->cursor_virtual_row = rs->virtual_row;
            rs->cursor_col = ctx->L.margin + 1;
        }
        rs->pos += hr_len;
    }
    rs->virtual_row++;
    rs->col_width = 0;
    return true;
}

//! Render header element with centered text and decorative underline
//! Used when text scaling is available for beautiful typography
static bool render_header_element(const RenderCtx *ctx, RenderState *rs,
                                  size_t header_content, size_t header_end,
                                  int header_level, MdStyle line_style) {
    int text_scale = get_line_scale(line_style);
    size_t header_total = header_end - rs->pos;
    if (header_end < ctx->len && gap_at(&app.text, header_end) == '\n') header_total++;

    bool cursor_in_header = cursor_in_range(app.cursor, rs->pos, rs->pos + header_total, app.hide_cursor_syntax);

    if (cursor_in_header) {
        // Editing mode: show raw markdown with scaling, left-aligned
        int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
        MdFracScale frac = md_get_frac_scale(line_style);
        current_text_scale = frac.scale;
        current_frac_num = frac.num;
        current_frac_denom = frac.denom;

        md_apply(line_style);

        // Render the raw header syntax including # prefix
        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
            move_to(screen_row, ctx->L.margin + 1);
        }

        // Track character position (unscaled) for wrapping calculation
        int char_col = 0;
        int available_width = ctx->L.text_width / text_scale;
        if (available_width < 1) available_width = 1;

        for (size_t p = rs->pos; p < header_end && p < ctx->len; ) {
            screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

            // Track cursor with scaled column position
            if (p == app.cursor) {
                rs->cursor_virtual_row = rs->virtual_row;
                rs->cursor_col = ctx->L.margin + 1 + (char_col * text_scale);
            }

            if (char_col >= available_width) {
                rs->virtual_row += text_scale;
                char_col = 0;
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                }
            }

            size_t next;
            int gw = grapheme_width_next(&app.text, p, &next);
            if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                output_grapheme(&app.text, &p);
            } else {
                p = next;
            }
            char_col += gw;
        }

        // Track cursor at newline position
        if (header_end < ctx->len && gap_at(&app.text, header_end) == '\n') {
            if (header_end == app.cursor) {
                rs->cursor_virtual_row = rs->virtual_row;
                rs->cursor_col = ctx->L.margin + 1 + (char_col * text_scale);
            }
        }
        rs->pos = header_end;
        if (rs->pos < ctx->len && gap_at(&app.text, rs->pos) == '\n') rs->pos++;

        rs->virtual_row += text_scale;
        rs->col_width = 0;
        rs->line_style = 0;
        current_text_scale = 1;
        current_frac_num = 0;
        current_frac_denom = 0;
        md_apply(0);
        return true;
    }

    // Beautiful mode: centered header with balanced word wrapping
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    // Skip any leading whitespace after the # prefix
    size_t content_start = header_content;
    while (content_start < header_end && gap_at(&app.text, content_start) == ' ') {
        content_start++;
    }

    // Also trim trailing whitespace
    size_t content_end = header_end;
    while (content_end > content_start && gap_at(&app.text, content_end - 1) == ' ') {
        content_end--;
    }

    MdFracScale frac = md_get_frac_scale(line_style);

    // Available width in character cells (not scaled cells)
    int available_char_width = ctx->L.text_width / text_scale;
    if (available_char_width < 1) available_char_width = 1;

    // Calculate total content width
    int total_content_width = 0;
    for (size_t p = content_start; p < content_end; ) {
        size_t next;
        total_content_width += grapheme_width_next(&app.text, p, &next);
        p = next;
    }

    // For balanced wrapping, find the optimal break point that creates
    // the most evenly sized lines. Collect all word break positions first.
    size_t break_positions[64];
    int break_widths[64];  // cumulative width at each break
    int break_count = 0;
    int cumulative_width = 0;

    for (size_t p = content_start; p < content_end && break_count < 63; ) {
        char c = gap_at(&app.text, p);
        size_t next;
        int gw = grapheme_width_next(&app.text, p, &next);
        cumulative_width += gw;
        p = next;

        // Record position after each space as a potential break point
        if (c == ' ') {
            break_positions[break_count] = p;
            break_widths[break_count] = cumulative_width;
            break_count++;
        }
    }

    // Find the break point that creates most balanced lines
    size_t best_break = content_end;
    int best_diff = total_content_width;  // worst case: all on one line

    if (total_content_width > available_char_width && break_count > 0) {
        for (int i = 0; i < break_count; i++) {
            int first_line_width = break_widths[i] - 1;  // exclude trailing space
            int second_line_width = total_content_width - break_widths[i];

            // Both lines must fit within available width
            if (first_line_width <= available_char_width && second_line_width <= available_char_width) {
                int diff = first_line_width > second_line_width
                         ? first_line_width - second_line_width
                         : second_line_width - first_line_width;
                if (diff < best_diff) {
                    best_diff = diff;
                    best_break = break_positions[i];
                }
            }
        }
    }

    // Word-wrap the header content into lines
    size_t line_start = content_start;
    while (line_start < content_end) {
        size_t line_end;      // where this line's content ends (for advancing)
        size_t render_end;    // where to stop rendering (excludes trailing space)
        int line_width;

        // Use the pre-calculated optimal break for first line if wrapping needed
        if (line_start == content_start && best_break < content_end) {
            line_end = best_break;
            render_end = line_end;
            // Trim trailing spaces from render end
            while (render_end > line_start && gap_at(&app.text, render_end - 1) == ' ') {
                render_end--;
            }
            // Calculate width of what we'll actually render
            line_width = 0;
            for (size_t p = line_start; p < render_end; ) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        } else {
            // For subsequent lines or single line, take everything remaining
            line_end = content_end;
            render_end = content_end;
            line_width = 0;
            for (size_t p = line_start; p < render_end; ) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        }

        // Skip leading spaces on continuation lines
        if (line_start > content_start) {
            while (line_start < line_end && gap_at(&app.text, line_start) == ' ') {
                line_start++;
            }
            render_end = line_end;  // render_end is same as line_end for continuation
            // Recalculate line width after trimming
            line_width = 0;
            for (size_t p = line_start; p < render_end; ) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        }

        // Calculate centering for this line
        int scaled_line_width = line_width * text_scale;
        int left_padding = (ctx->L.text_width - scaled_line_width) / 2;
        if (left_padding < 0) left_padding = 0;

        // Render this line (only up to render_end, excluding trailing spaces)
        screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
            current_text_scale = frac.scale;
            current_frac_num = frac.num;
            current_frac_denom = frac.denom;

            md_apply(line_style);

            move_to(screen_row, ctx->L.margin + 1 + left_padding);

            for (size_t p = line_start; p < render_end; ) {
                output_grapheme(&app.text, &p);
            }

            // Draw decorative underline on separate row for H2+ headers (only after last line)
            bool is_last_line = (line_end >= content_end);
            if (header_level > 1 && is_last_line) {
                // Reset text scale for underline
                current_text_scale = 1;
                current_frac_num = 0;
                current_frac_denom = 0;
                md_apply(0);

                int underline_row = screen_row + text_scale;
                if (is_row_visible(&ctx->L, underline_row, ctx->max_row)) {
                    // Decorative underline is ~1/3 width of text, centered
                    int underline_width = scaled_line_width / 3;
                    if (underline_width < 4) underline_width = 4;
                    int underline_padding = left_padding + (scaled_line_width - underline_width) / 2;

                    move_to(underline_row, ctx->L.margin + 1 + underline_padding);
                    set_fg(get_dim());
                    for (int i = 0; i < underline_width; i++) {
                        out_str("─");
                    }
                    set_fg(get_fg());
                }
                rs->virtual_row++;  // account for underline row
            }
        }

        // Track cursor position
        if (app.cursor >= rs->pos && app.cursor < rs->pos + header_total) {
            rs->cursor_virtual_row = rs->virtual_row;
            rs->cursor_col = ctx->L.margin + 1 + left_padding;
        }

        rs->virtual_row += text_scale;
        line_start = line_end;
    }

    // Skip to end of header
    rs->pos = header_end;
    if (rs->pos < ctx->len && gap_at(&app.text, rs->pos) == '\n') rs->pos++;

    rs->col_width = 0;
    rs->line_style = 0;
    current_text_scale = 1;
    current_frac_num = 0;
    current_frac_denom = 0;
    md_apply(0);

    (void)header_level;
    return true;
}

//! Render code block element
static bool render_code_block_element(const RenderCtx *ctx, RenderState *rs,
                                      size_t cb_lang_start, size_t cb_lang_len,
                                      size_t cb_content_start, size_t cb_content_len,
                                      size_t cb_total_len) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
    
    if (cursor_in_range(app.cursor, rs->pos, rs->pos + cb_total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row,
                                rs->pos + cb_total_len);
    } else {
        char lang[32] = {0};
        if (cb_lang_len > 0 && cb_lang_len < sizeof(lang)) {
            for (size_t i = 0; i < cb_lang_len; i++) {
                lang[i] = gap_at(&app.text, cb_lang_start + i);
            }
        }
        
        char *code = malloc(cb_content_len + 1);
        if (code) {
            for (size_t i = 0; i < cb_content_len; i++) {
                code[i] = gap_at(&app.text, cb_content_start + i);
            }
            code[cb_content_len] = '\0';
            
            size_t hl_len = 0;
            char *highlighted = highlight_code(app.hl_ctx, code, cb_content_len,
                                               lang[0] ? lang : NULL, &hl_len);
            
            const char *hl = highlighted ? highlighted : code;
            const char *p = hl;
            bool first_line = true;
            int vis_col = 0;
            
            while (*p || first_line) {
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                if (screen_row > ctx->max_row) break;
                
                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                    set_bg(get_code_bg());
                    out_spaces(ctx->L.text_width);
                    
                    if (first_line && lang[0]) {
                        int label_len = (int)strlen(lang);
                        move_to(screen_row, ctx->L.margin + 1 + ctx->L.text_width - label_len - 1);
                        set_bg(get_code_bg());
                        set_fg(get_dim());
                        out_str(lang);
                    }
                    
                    move_to(screen_row, ctx->L.margin + 1);
                    set_bg(get_code_bg());
                }
                
                vis_col = 0;
                while (*p && *p != '\n') {
                    if (*p == '\x1b' && *(p+1) == '[') {
                        const char *seq_start = p;
                        p += 2;
                        while (*p && *p != 'm') p++;
                        if (*p == 'm') p++;
                        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                            out_str_n(seq_start, (size_t)(p - seq_start));
                            set_bg(get_code_bg());
                        }
                    } else {
                        int char_width = 1, char_bytes = 1;
                        uint8_t c = (uint8_t)*p;
                        if (c == '\t') {
                            int tab_width = 4 - (vis_col % 4);
                            if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                                for (int ti = 0; ti < tab_width; ti++) {
                                    out_char(' ');
                                }
                            }
                            vis_col += tab_width;
                            p++;
                            continue;
                        }

                        if (c >= 0x80) {
                            utf8proc_int32_t cp;
                            utf8proc_ssize_t bytes = utf8proc_iterate((const utf8proc_uint8_t *)p, -1, &cp);
                            if (bytes > 0 && cp >= 0) {
                                char_width = utf8proc_charwidth(cp);
                                if (char_width < 0) char_width = 1;
                                char_bytes = (int)bytes;
                            }
                        }
                        if (vis_col + char_width > ctx->L.text_width) break;
                        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                            if (char_bytes > 1) out_str_n(p, (size_t)char_bytes);
                            else out_char(*p);
                        }
                        vis_col += char_width;
                        p += char_bytes;
                    }
                }

                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    reset_attrs();
                    set_bg(get_bg());
                }

                first_line = false;
                rs->virtual_row++;
                if (*p == '\n') p++;
            }
            
            free(highlighted);
            free(code);
        }
        rs->pos += cb_total_len;
    }
    rs->col_width = 0;
    return true;
}

//! Render block math element
static bool render_block_math_element(const RenderCtx *ctx, RenderState *rs,
                                      size_t bm_content_start, size_t bm_content_len,
                                      size_t bm_total_len) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
    
    if (cursor_in_range(app.cursor, rs->pos, rs->pos + bm_total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row,
                                rs->pos + bm_total_len);
    } else {
        char *latex = malloc(bm_content_len + 1);
        if (latex) {
            for (size_t i = 0; i < bm_content_len; i++) {
                latex[i] = gap_at(&app.text, bm_content_start + i);
            }
            latex[bm_content_len] = '\0';
            
            TexSketch *sketch = tex_render_string(latex, bm_content_len, true);
            free(latex);
            
            if (sketch) {
                for (int r = 0; r < sketch->height; r++) {
                    screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                        move_to(screen_row, ctx->L.margin + 1);
                        set_fg(get_accent());
                        for (int c = 0; c < sketch->rows[r].count; c++) {
                            if (sketch->rows[r].cells[c].data) {
                                out_str(sketch->rows[r].cells[c].data);
                            }
                        }
                        set_fg(get_fg());
                    }
                    rs->virtual_row++;
                }
                tex_sketch_free(sketch);
            }
        }
        rs->pos += bm_total_len;
    }
    rs->col_width = 0;
    return true;
}

//! Render table element
static bool render_table_element(const RenderCtx *ctx, RenderState *rs, const MdTable *tbl) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
    
    if (cursor_in_range(app.cursor, rs->pos, rs->pos + tbl->total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row,
                                rs->pos + tbl->total_len);
    } else {
        int col_widths[MD_TABLE_MAX_COLS];
        calc_table_col_widths(tbl->col_count, ctx->L.text_width, col_widths);
        
        size_t row_starts[64], row_lens[64];
        int row_count = 0;
        
        // Parse all rows
        size_t scan_pos = rs->pos;
        while (scan_pos < rs->pos + tbl->total_len && row_count < 64) {
            int scan_cols = 0;
            size_t scan_len = 0;
            
            if (row_count == 1) {
                MdAlign dummy_align[MD_TABLE_MAX_COLS];
                if (md_check_table_delimiter(&app.text, scan_pos, &scan_cols, dummy_align, &scan_len)) {
                    row_starts[row_count] = scan_pos;
                    row_lens[row_count] = scan_len;
                    row_count++;
                    scan_pos += scan_len;
                    continue;
                }
            }
            
            if (md_check_table_header(&app.text, scan_pos, &scan_cols, &scan_len)) {
                row_starts[row_count] = scan_pos;
                row_lens[row_count] = scan_len;
                row_count++;
                scan_pos += scan_len;
            } else {
                break;
            }
        }
        
        // Calculate row heights
        int row_heights[64] = {0};
        for (int ri = 0; ri < row_count; ri++) {
            if (ri == 1) { row_heights[ri] = 1; continue; }
            
            size_t cell_starts[MD_TABLE_MAX_COLS], cell_lens[MD_TABLE_MAX_COLS];
            int cells = md_parse_table_row(&app.text, row_starts[ri], row_lens[ri],
                                           cell_starts, cell_lens, MD_TABLE_MAX_COLS);
            int max_lines = 1;
            for (int ci = 0; ci < cells && ci < tbl->col_count; ci++) {
                int cell_lines = calc_cell_wrapped_lines(cell_starts[ci], cell_lens[ci], col_widths[ci]);
                if (cell_lines > max_lines) max_lines = cell_lines;
            }
            row_heights[ri] = max_lines;
        }
        
        // Top border
        screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
        render_table_hborder(&ctx->L, screen_row, ctx->max_row, tbl->col_count, col_widths, "┌", "┬", "┐");
        rs->virtual_row++;
        
        // Render rows
        for (int ri = 0; ri < row_count; ri++) {
            if (ri == 1) {
                // Delimiter row
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                render_table_hborder(&ctx->L, screen_row, ctx->max_row, tbl->col_count, col_widths, "├", "┼", "┤");
                rs->virtual_row++;
                continue;
            }
            
            size_t cell_starts[MD_TABLE_MAX_COLS], cell_lens[MD_TABLE_MAX_COLS];
            int cells = md_parse_table_row(&app.text, row_starts[ri], row_lens[ri],
                                           cell_starts, cell_lens, MD_TABLE_MAX_COLS);
            
            size_t cell_render_pos[MD_TABLE_MAX_COLS];
            MdStyle cell_active_styles[MD_TABLE_MAX_COLS];
            struct { MdStyle style; size_t dlen; } cell_stacks[MD_TABLE_MAX_COLS][8];
            int cell_stack_depths[MD_TABLE_MAX_COLS];
            
            for (int ci = 0; ci < tbl->col_count; ci++) {
                cell_render_pos[ci] = (ci < cells) ? cell_starts[ci] : 0;
                cell_active_styles[ci] = 0;
                cell_stack_depths[ci] = 0;
            }
            
            for (int line = 0; line < row_heights[ri]; line++) {
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                
                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                    set_fg(get_border());
                    out_str("│");
                    
                    for (int ci = 0; ci < tbl->col_count; ci++) {
                        size_t cell_end = (ci < cells) ? cell_starts[ci] + cell_lens[ci] : 0;
                        bool is_header = (ri == 0);
                        MdAlign align = (ci < tbl->col_count) ? tbl->align[ci] : MD_ALIGN_DEFAULT;
                        
                        // Measure content width
                        size_t measure_pos = cell_render_pos[ci];
                        int content_width = 0;
                        while (measure_pos < cell_end && content_width < col_widths[ci]) {
                            size_t dlen = 0;
                            MdStyle delim = md_check_delim(&app.text, measure_pos, &dlen);
                            if (delim != 0 && dlen > 0) { measure_pos += dlen; continue; }
                            size_t next_pos;
                            int gw = grapheme_width_next(&app.text, measure_pos, &next_pos);
                            if (content_width + gw > col_widths[ci]) break;
                            content_width += gw;
                            measure_pos = next_pos;
                        }
                        
                        int padding = col_widths[ci] - content_width;
                        if (padding < 0) padding = 0;
                        int left_pad = 0, right_pad = padding;
                        
                        switch (align) {
                            case MD_ALIGN_RIGHT: left_pad = padding; right_pad = 0; break;
                            case MD_ALIGN_CENTER: left_pad = padding / 2; right_pad = padding - left_pad; break;
                            default: break;
                        }
                        
                        reset_attrs(); set_bg(get_bg());
                        out_char(' ');
                        for (int p = 0; p < left_pad; p++) out_char(' ');
                        
                        if (is_header) set_bold(true);
                        if (cell_active_styles[ci]) md_apply(cell_active_styles[ci]);
                        else set_fg(get_fg());
                        
                        int rendered_width = 0;
                        while (cell_render_pos[ci] < cell_end && rendered_width < col_widths[ci]) {
                            size_t dlen = 0;
                            MdStyle delim = md_check_delim(&app.text, cell_render_pos[ci], &dlen);
                            
                            if (delim != 0 && dlen > 0) {
                                bool closed = false;
                                for (int si = cell_stack_depths[ci] - 1; si >= 0; si--) {
                                    if (cell_stacks[ci][si].style == delim && cell_stacks[ci][si].dlen == dlen) {
                                        cell_active_styles[ci] &= ~delim;
                                        cell_stack_depths[ci] = si;
                                        closed = true;
                                        break;
                                    }
                                }
                                if (!closed && cell_stack_depths[ci] < 8) {
                                    cell_stacks[ci][cell_stack_depths[ci]].style = delim;
                                    cell_stacks[ci][cell_stack_depths[ci]].dlen = dlen;
                                    cell_stack_depths[ci]++;
                                    cell_active_styles[ci] |= delim;
                                }
                                cell_render_pos[ci] += dlen;
                                reset_attrs(); set_bg(get_bg());
                                if (is_header) set_bold(true);
                                if (cell_active_styles[ci]) md_apply(cell_active_styles[ci]);
                                else set_fg(get_fg());
                                continue;
                            }
                            
                            size_t next_pos;
                            int gw = grapheme_width_next(&app.text, cell_render_pos[ci], &next_pos);
                            if (rendered_width + gw > col_widths[ci]) break;
                            
                            for (size_t j = cell_render_pos[ci]; j < next_pos && j < cell_end; j++) {
                                out_char(gap_at(&app.text, j));
                            }
                            rendered_width += gw;
                            cell_render_pos[ci] = next_pos;
                        }
                        
                        reset_attrs(); set_bg(get_bg());
                        for (int p = 0; p < right_pad; p++) out_char(' ');
                        out_char(' ');
                        
                        set_fg(get_border());
                        out_str("│");
                    }
                }
                rs->virtual_row++;
            }
            
            // Row divider
            if (ri < row_count - 1 && ri != 0) {
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                render_table_hborder(&ctx->L, screen_row, ctx->max_row, tbl->col_count, col_widths, "├", "┼", "┤");
                rs->virtual_row++;
            }
        }
        
        // Bottom border
        screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
        render_table_hborder(&ctx->L, screen_row, ctx->max_row, tbl->col_count, col_widths, "└", "┴", "┘");
        rs->virtual_row++;
        
        rs->pos += tbl->total_len;
    }
    rs->col_width = 0;
    return true;
}

// #endregion

// #region AI Panel Rendering

//! Render AI panel
static void render_ai_panel(const Layout *L) {
    int padding = 1;
    int prefix_len = 4;
    int content_start = L->ai_start_col + 1 + padding;
    int content_width = L->ai_cols - 1 - (padding * 2);
    int first_line_width = content_width - prefix_len;
    int cont_line_width = content_width - prefix_len;
    if (first_line_width < 10) first_line_width = 10;
    if (cont_line_width < 10) cont_line_width = 10;
    
    if (!app.ai_focused) set_dim(true);
    
    // Draw border and clear
    for (int row = 1; row <= app.rows; row++) {
        move_to(row, L->ai_start_col);
        set_bg(get_ai_bg());
        set_fg(get_border());
        out_str("│");
        out_spaces(L->ai_cols - 1);
    }
    
    // Header
    move_to(1, L->ai_start_col + 1);
    set_bg(get_ai_bg());
    out_spaces(padding);
    set_fg(get_fg());
    set_bold(true);
    out_str("chat");
    reset_attrs();
    set_bg(get_ai_bg());
    
    // Header separator
    move_to(2, L->ai_start_col);
    set_bg(get_ai_bg());
    set_fg(get_border());
    out_str("├");
    for (int ic = 0; ic < L->ai_cols - 2; ic++) out_str("─");
    
    // Hint
    const char *hint = "esc close";
    int hint_col = L->ai_start_col + L->ai_cols - (int)strlen(hint) - padding - 1;
    move_to(1, hint_col);
    set_bg(get_ai_bg());
    set_fg(get_dim());
    out_str(hint);
    
    // Calculate input area
    int input_width = content_width - 2;
    int input_lines = 1, icol = 0;
    for (size_t i = 0; i < app.ai_input_len; i++) {
        if (app.ai_input[i] == '\n') { input_lines++; icol = 0; }
        else { icol++; if (icol >= input_width) { input_lines++; icol = 0; } }
    }
    if (input_lines > AI_INPUT_MAX_LINES) input_lines = AI_INPUT_MAX_LINES;
    
    int input_start_row = app.rows - input_lines;
    int msg_area_start = 4;
    int msg_area_end = input_start_row - 2;
    int msg_area_height = msg_area_end - msg_area_start;
    if (msg_area_height < 1) msg_area_height = 1;
    
    // Calculate message lines
    int total_lines = 0;
    int *msg_start_lines = NULL, *msg_line_counts = NULL;
    int max_scroll = 0;

    if (app.chat_count > 0) {
        msg_start_lines = malloc(sizeof(int) * (size_t)app.chat_count);
        msg_line_counts = malloc(sizeof(int) * (size_t)app.chat_count);
        if (!msg_start_lines || !msg_line_counts) {
            free(msg_start_lines); free(msg_line_counts);
            goto skip_chat;
        }
        
        for (int i = 0; i < app.chat_count; i++) {
            msg_start_lines[i] = total_lines;
            ChatMessage *m = &app.chat_msgs[i];
            
            int lines = 0;
            size_t pos = 0;
            while (pos < m->len) {
                int width = (lines == 0) ? first_line_width : cont_line_width;
                int chars = chat_wrap_line(m->text, m->len, pos, width);
                if (chars == 0) break;
                if (chars == -1) { lines++; pos++; continue; }
                lines++;
                pos += (size_t)chars;
                if (pos < m->len && (m->text[pos] == '\n' || m->text[pos] == ' ')) pos++;
            }
            if (lines == 0) lines = 1;
            
            msg_line_counts[i] = lines;
            total_lines += lines + 1;
        }
    }
    
    int thinking_line = -1;
    if (app.ai_thinking) { thinking_line = total_lines; total_lines++; }

    max_scroll = total_lines > msg_area_height ? total_lines - msg_area_height : 0;
    if (app.chat_scroll < 0) app.chat_scroll = 0;
    if (app.chat_scroll > max_scroll) app.chat_scroll = max_scroll;
    
    int first_visible = max_scroll - app.chat_scroll;
    if (first_visible < 0) first_visible = 0;
    int last_visible = first_visible + msg_area_height;
    
    // Render messages
    int screen_row = msg_area_start;
    
    for (int i = 0; i < app.chat_count && screen_row < msg_area_end; i++) {
        ChatMessage *m = &app.chat_msgs[i];
        int msg_start = msg_start_lines[i];
        int msg_lines = msg_line_counts[i];
        
        if (msg_start + msg_lines < first_visible) continue;
        if (msg_start >= last_visible) break;
        
        size_t pos = 0;
        int line_in_msg = 0;
        
        while (pos < m->len && screen_row < msg_area_end) {
            int global_line = msg_start + line_in_msg;
            bool visible = (global_line >= first_visible && global_line < last_visible);
            
            int width = (line_in_msg == 0) ? first_line_width : cont_line_width;
            int chars = chat_wrap_line(m->text, m->len, pos, width);
            if (chars == 0) break;
            if (chars == -1) {
                if (visible) screen_row++;
                pos++; line_in_msg++;
                continue;
            }
            
            if (visible) {
                move_to(screen_row, content_start);
                set_bg(get_ai_bg());
                
                if (line_in_msg == 0) {
                    if (m->is_user) { set_fg(get_accent()); out_str("you "); }
                    else { set_fg(get_dim()); out_str("ai  "); }
                } else {
                    out_str("    ");
                }
                
                set_fg(get_fg());
                if (m->is_user) {
                    for (int c = 0; c < chars; c++) out_char(m->text[pos + c]);
                } else {
                    chat_print_md(m->text, pos, chars);
                }
                screen_row++;
            }
            
            pos += (size_t)chars;
            if (pos < m->len && (m->text[pos] == '\n' || m->text[pos] == ' ')) pos++;
            line_in_msg++;
        }
        
        if (m->len == 0 && !(app.ai_thinking && !m->is_user)) {
            int global_line = msg_start;
            if (global_line >= first_visible && global_line < last_visible) {
                move_to(screen_row, content_start);
                set_bg(get_ai_bg());
                if (m->is_user) { set_fg(get_accent()); out_str("you "); }
                else { set_fg(get_dim()); out_str("ai  "); }
                screen_row++;
            }
        }
        
        int blank_line = msg_start + msg_lines;
        if (blank_line >= first_visible && blank_line < last_visible) screen_row++;
    }
    
    // Thinking indicator
    if (app.ai_thinking && thinking_line >= first_visible && thinking_line < last_visible && screen_row < msg_area_end) {
        move_to(screen_row, content_start);
        set_bg(get_ai_bg());
        set_fg(get_dim());
        out_str("ai  ");
        const PlatformBackend *p = platform_get();
        int64_t now = p && p->time_now ? p->time_now() : 0;
        int phase = (int)(now % 4);
        const char *dots[] = {"·  ", "·· ", "···", "   "};
        out_str(dots[phase]);
    }
    
    free(msg_start_lines);
    free(msg_line_counts);
    
skip_chat:
    // Scroll indicator
    if (max_scroll > 0 && app.chat_scroll > 0) {
        move_to(3, content_start);
        set_fg(get_dim());
        set_bg(get_ai_bg());
        out_str("↑ scroll for more");
    }
    
    // Input separator
    move_to(input_start_row - 1, content_start);
    set_bg(get_ai_bg());
    set_fg(get_border());
    for (int ic = 0; ic < content_width; ic++) out_str("─");
    
    // Input area
    move_to(input_start_row, content_start);
    set_bg(get_ai_bg());
    set_fg(get_accent());
    out_str("> ");
    set_fg(get_fg());
    
    int cur_row = input_start_row, cur_col = 2;
    int cursor_row = input_start_row, cursor_col = content_start + 2;
    
    for (size_t i = 0; i < app.ai_input_len && cur_row <= app.rows; i++) {
        if (i == app.ai_input_cursor) {
            cursor_row = cur_row;
            cursor_col = content_start + cur_col;
        }
        
        char c = app.ai_input[i];
        if (c == '\n') {
            cur_row++; cur_col = 0;
            if (cur_row <= app.rows) { move_to(cur_row, content_start); set_bg(get_ai_bg()); }
            continue;
        }
        
        if (cur_col >= input_width + 2) {
            cur_row++; cur_col = 0;
            if (cur_row > app.rows) break;
            move_to(cur_row, content_start);
            set_bg(get_ai_bg());
        }
        
        out_char(c);
        cur_col++;
    }
    
    if (app.ai_input_cursor >= app.ai_input_len) {
        cursor_row = cur_row;
        cursor_col = content_start + cur_col;
    }
    
    if (app.ai_focused) {
        move_to(cursor_row, cursor_col);
        cursor_visible(true);
    }
    reset_attrs();
}

// #endregion

// #region Status Bar Rendering

//! Render status bar
static void render_status_bar(const Layout *L) {
    int words = count_words(&app.text);
    int status_left = L->margin + 1;
    int status_right = L->margin + L->text_width;
    
    move_to(app.rows, 1);
    for (int i = 0; i < L->text_area_cols; i++) out_char(' ');
    
    move_to(app.rows, status_left);
    set_fg(get_dim());
    
    bool need_sep = false;
    
    if (app.timer_mins > 0 && app.timer_on) {
        int rem = timer_remaining();
        float prog = (float)rem / (app.timer_mins * 60.0f);
        Color tc = color_lerp(get_dim(), get_accent(), prog);
        set_fg(tc);
        if (app.timer_paused) out_str("⏸ ");
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", rem / 60, rem % 60);
        out_str(time_buf);
        need_sep = true;
    }
    
    if (need_sep) { set_fg(get_border()); out_str(" · "); }
    set_fg(get_dim());
    char words_buf[32];
    snprintf(words_buf, sizeof(words_buf), "%d word%s", words, words == 1 ? "" : "s");
    out_str(words_buf);
    
    if (app.focus_mode) {
        set_fg(get_border()); out_str(" · ");
        set_fg(get_accent()); out_str("focus");
    }
    
    if (has_selection()) {
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        set_fg(get_border()); out_str(" · ");
        set_fg(get_dim());
        char sel_buf[32];
        snprintf(sel_buf, sizeof(sel_buf), "%zu sel", sel_e - sel_s);
        out_str(sel_buf);
    }
    
    // Right side hints
    char hints[64] = "";
    int hints_len = 0;
    
    if (app.timer_on) {
        hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "^P");
    }
    #if HAS_LIBAI
    if (app.ai_ready) {
        if (hints_len > 0) hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, " · ");
        hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "^/");
    }
    #endif
    if (hints_len > 0) hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, " · ");
    snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "esc");
    
    int hints_col = status_right - (int)strlen(hints) + 1;
    if (hints_col > status_left + 20) {
        move_to(app.rows, hints_col);
        set_fg(get_dim());
        out_str(hints);
    }
}

// #endregion

// #region Inline Element Rendering

//! Render raw dimmed content for cursor-in-element case (common helper for inline elements)
static void render_cursor_in_element(const RenderCtx *ctx, RenderState *rs, size_t element_len) {
    set_fg(get_dim());
    size_t end_pos = rs->pos + element_len;
    while (rs->pos < end_pos && rs->pos < ctx->len) {
        int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
        track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                    &rs->cursor_virtual_row, &rs->cursor_col);
        wrap_and_render_grapheme(ctx, &rs->pos, &rs->col_width, &rs->virtual_row, &screen_row);
    }
    set_fg(get_fg());
}

//! Render inline math element
static bool render_inline_math(const RenderCtx *ctx, RenderState *rs,
                               size_t math_content_start, size_t math_content_len,
                               size_t math_total) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + math_total)) {
        render_cursor_in_element(ctx, rs, math_total);
        return true;
    }
    
    char *latex = malloc(math_content_len + 1);
    if (latex) {
        for (size_t i = 0; i < math_content_len; i++) {
            latex[i] = gap_at(&app.text, math_content_start + i);
        }
        latex[math_content_len] = '\0';
        
        TexSketch *sketch = tex_render_inline(latex, math_content_len, true);
        free(latex);

        if (sketch && sketch->height == 1) {
            rs->pos += math_total;
            if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                set_fg(get_accent());
                for (int c = 0; c < sketch->rows[0].count; c++) {
                    if (sketch->rows[0].cells[c].data) {
                        out_str(sketch->rows[0].cells[c].data);
                    }
                }
                set_fg(get_fg());
            }
            rs->col_width += sketch->width;
            tex_sketch_free(sketch);
            return true;
        } else if (sketch && sketch->height > 1) {
            // Multi-row inline math - position at current column
            int start_col = ctx->L.margin + 1 + rs->col_width;
            rs->pos += math_total;
            for (int r = 0; r < sketch->height; r++) {
                screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, start_col);
                    set_fg(get_accent());
                    for (int c = 0; c < sketch->rows[r].count; c++) {
                        if (sketch->rows[r].cells[c].data) {
                            out_str(sketch->rows[r].cells[c].data);
                        }
                    }
                    set_fg(get_fg());
                }
                rs->virtual_row++;
            }
            rs->col_width += sketch->width;
            tex_sketch_free(sketch);
            return true;
        }
        if (sketch) tex_sketch_free(sketch);
    }
    
    // Fallback
    rs->pos += math_total;
    if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
        set_fg(get_accent());
        set_italic(true);
        for (size_t i = 0; i < math_content_len; i++) out_char(gap_at(&app.text, math_content_start + i));
        reset_attrs();
        set_bg(get_bg());
        set_fg(get_fg());
    }
    rs->col_width += (int)math_content_len;
    return true;
}

//! Render link element
static bool render_link(const RenderCtx *ctx, RenderState *rs,
                        size_t link_text_start, size_t link_text_len,
                        size_t link_url_start, size_t link_url_len,
                        size_t link_total) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + link_total)) {
        render_cursor_in_element(ctx, rs, link_total);
        return true;
    }
    
    char url[1024];
    size_t ulen = link_url_len < sizeof(url) - 1 ? link_url_len : sizeof(url) - 1;
    gap_copy_to(&app.text, link_url_start, ulen, url);
    url[ulen] = '\0';
    rs->pos += link_total;
    
    if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
        char link_seq[1100];
        snprintf(link_seq, sizeof(link_seq), "\x1b]8;;%s\x1b\\", url);
        out_str(link_seq);
        set_underline(UNDERLINE_STYLE_SINGLE);
        set_fg(get_accent());
        
        size_t link_pos = link_text_start;
        size_t link_end = link_text_start + link_text_len;
        int link_display_width = 0;
        bool in_code = false;
        
        while (link_pos < link_end) {
            char ch = gap_at(&app.text, link_pos);
            if (ch == '`') {
                in_code = !in_code;
                link_pos++;
                set_dim(in_code);
                continue;
            }
            
            size_t next_pos;
            int gw = grapheme_width_next(&app.text, link_pos, &next_pos);
            for (size_t j = link_pos; j < next_pos && j < link_end; j++) {
                out_char(gap_at(&app.text, j));
            }
            link_display_width += gw;
            link_pos = next_pos;
        }
        
        clear_underline();
        reset_attrs();
        out_str("\x1b]8;;\x1b\\");
        set_bg(get_bg());
        set_fg(get_fg());
        rs->col_width += link_display_width;
    } else {
        rs->col_width += gap_display_width(&app.text, link_text_start, link_text_start + link_text_len);
    }
    return true;
}

//! Render footnote reference
static bool render_footnote_ref(const RenderCtx *ctx, RenderState *rs,
                                size_t fnref_id_start, size_t fnref_id_len,
                                size_t fnref_total) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + fnref_total)) {
        render_cursor_in_element(ctx, rs, fnref_total);
    } else {
        rs->pos += fnref_total;
        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
            set_fg(get_accent());
            out_str("[");
            for (size_t i = 0; i < fnref_id_len; i++) out_char(gap_at(&app.text, fnref_id_start + i));
            out_str("]");
            set_fg(get_fg());
        }
        rs->col_width += (int)fnref_id_len + 2;
    }
    return true;
}

//! Render heading ID (hidden unless cursor is inside)
static bool render_heading_id(const RenderCtx *ctx, RenderState *rs,
                              size_t hid_start, size_t hid_len, size_t hid_total) {
    (void)hid_start; (void)hid_len;

    if (CURSOR_IN(rs->pos, rs->pos + hid_total)) {
        render_cursor_in_element(ctx, rs, hid_total);
    } else {
        rs->pos += hid_total;
    }
    return true;
}

//! Render emoji shortcode
static bool render_emoji(const RenderCtx *ctx, RenderState *rs,
                        const char *emoji, size_t emoji_total) {
    int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + emoji_total)) {
        render_cursor_in_element(ctx, rs, emoji_total);
    } else {
        rs->pos += emoji_total;
        if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
            out_str(emoji);
        }
        rs->col_width += 2;
    }
    return true;
}

// #endregion

// #region Line Prefix Rendering

//! Render line prefix elements (task, header, list, quote, footnote def)
static void render_line_prefixes(const RenderCtx *ctx, RenderState *rs,
                                 size_t line_end, size_t *seg_end, int *seg_width) {
    size_t len = ctx->len;
    int text_scale = get_line_scale(rs->line_style);
    
    // Task list
    size_t task_content;
    int task_indent;
    int task_state = md_check_task(&app.text, rs->pos, &task_content, &task_indent);
    if (task_state > 0) {
        if (CURSOR_IN(rs->pos, task_content)) {
            render_raw_prefix(&rs->pos, task_content, &rs->col_width, len,
                             &rs->cursor_virtual_row, &rs->cursor_col, rs->virtual_row, ctx->L.margin);
        } else {
            rs->pos = task_content;
            set_fg(get_dim());
            for (int i = 0; i < task_indent; i++) { out_char(' '); rs->col_width++; }
            if (task_state == 2) out_str("☑ ");
            else out_str("☐ ");
            set_fg(get_fg());
            rs->col_width += 2;
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
    }
    
    // Header prefix
    size_t header_content;
    int header_level = md_check_header_content(&app.text, rs->pos, &header_content);
    if (header_level > 0) {
        if (CURSOR_IN(rs->pos, header_content)) {
            MdFracScale frac = md_get_frac_scale(rs->line_style);
            current_text_scale = frac.scale;
            current_frac_num = frac.num;
            current_frac_denom = frac.denom;
            render_raw_prefix(&rs->pos, header_content, &rs->col_width, len,
                             &rs->cursor_virtual_row, &rs->cursor_col, rs->virtual_row, ctx->L.margin);
        } else {
            rs->pos = header_content;
        }
        int header_scale = HAS_CAP(PLATFORM_CAP_TEXT_SIZING) ? text_scale : 1;
        int available = (ctx->L.text_width - rs->col_width) / header_scale;
        if (available < 1) available = 1;
        *seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, available, seg_width);
    }
    
    // List
    size_t list_content;
    int list_indent;
    int list_type = md_check_list(&app.text, rs->pos, &list_content, &list_indent);
    if (list_type > 0 && task_state == 0 && header_level == 0) {
        if (CURSOR_IN(rs->pos, list_content)) {
            render_raw_prefix(&rs->pos, list_content, &rs->col_width, len,
                             &rs->cursor_virtual_row, &rs->cursor_col, rs->virtual_row, ctx->L.margin);
        } else {
            set_fg(get_dim());
            for (int i = 0; i < list_indent; i++) { out_char(' '); rs->col_width++; }
            if (list_type == 1) {
                out_str("• "); rs->col_width += 2;
            } else {
                size_t p = rs->pos + list_indent;
                int num = 0;
                while (p < len && gap_at(&app.text, p) >= '0' && gap_at(&app.text, p) <= '9') {
                    num = num * 10 + (gap_at(&app.text, p) - '0');
                    p++;
                }
                char num_buf[16];
                int printed = snprintf(num_buf, sizeof(num_buf), "%d. ", num);
                out_str(num_buf);
                rs->col_width += printed;
            }
            set_fg(get_fg());
            rs->pos = list_content;
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
    }
    
    // Blockquote
    size_t quote_content;
    int quote_level = md_check_blockquote(&app.text, rs->pos, &quote_content);
    if (quote_level > 0) {
        if (CURSOR_IN(rs->pos, quote_content)) {
            render_raw_prefix(&rs->pos, quote_content, &rs->col_width, len,
                             &rs->cursor_virtual_row, &rs->cursor_col, rs->virtual_row, ctx->L.margin);
        } else {
            rs->pos = quote_content;
            set_fg(get_accent());
            for (int i = 0; i < quote_level; i++) {
                out_str("┃ ");
                rs->col_width += 2;
            }
            set_fg(get_fg());
            set_italic(true);
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
    }
    
    // Footnote definition
    size_t fn_id_start, fn_id_len, fn_content, fn_total;
    if (md_check_footnote_def(&app.text, rs->pos, &fn_id_start, &fn_id_len, &fn_content, &fn_total)) {
        if (CURSOR_IN(rs->pos, fn_content)) {
            render_raw_prefix(&rs->pos, fn_content, &rs->col_width, len,
                             &rs->cursor_virtual_row, &rs->cursor_col, rs->virtual_row, ctx->L.margin);
        } else {
            rs->pos = fn_content;
            set_fg(get_accent());
            out_str("[");
            for (size_t i = 0; i < fn_id_len; i++) out_char(gap_at(&app.text, fn_id_start + i));
            out_str("] ");
            set_fg(get_fg());
            rs->col_width += (int)fn_id_len + 3;
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
    }
}

// #endregion

// #region Plain Mode Rendering

static WrapResult plain_wrap_cache = {0};
static size_t plain_wrap_text_len = 0;
static int plain_wrap_width = 0;

static void render_writing_plain(void) {
    set_bg(get_bg());
    cursor_home();
    
    for (int r = 0; r < app.rows; r++) {
        move_to(r + 1, 1);
        out_spaces(app.cols);
    }
    
    Layout L = calc_layout();
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    size_t len = gap_len(&app.text);
    
    if (plain_wrap_cache.lines == NULL || plain_wrap_text_len != len || plain_wrap_width != L.text_width) {
        if (plain_wrap_cache.lines) wrap_free(&plain_wrap_cache);
        wrap_init(&plain_wrap_cache);
        wrap_text(&app.text, L.text_width, &plain_wrap_cache);
        plain_wrap_text_len = len;
        plain_wrap_width = L.text_width;
    }
    WrapResult *wr = &plain_wrap_cache;
    
    int cursor_vrow = 0, cursor_col_in_line = 0;
    for (int i = 0; i < wr->count; i++) {
        if (app.cursor >= wr->lines[i].start && app.cursor <= wr->lines[i].end) {
            cursor_vrow = i;
            cursor_col_in_line = gap_display_width(&app.text, wr->lines[i].start, app.cursor);
            break;
        }
        if (app.cursor < wr->lines[i].start) { cursor_vrow = i > 0 ? i - 1 : 0; break; }
        cursor_vrow = i;
    }
    if (app.cursor >= len && wr->count > 0) {
        cursor_vrow = wr->count - 1;
        cursor_col_in_line = gap_display_width(&app.text, wr->lines[cursor_vrow].start, len);
    }
    
    // Adjust scroll with margin
    int scroll_margin = L.text_height > 10 ? 3 : 1;
    if (cursor_vrow < app.scroll_y + scroll_margin) {
        app.scroll_y = cursor_vrow - scroll_margin;
    }
    if (cursor_vrow >= app.scroll_y + L.text_height - scroll_margin) {
        app.scroll_y = cursor_vrow - L.text_height + scroll_margin + 1;
    }
    if (app.scroll_y < 0) app.scroll_y = 0;

    int cursor_screen_row = L.top_margin, cursor_screen_col = L.margin + 1;
    set_fg(get_fg());
    
    for (int i = app.scroll_y; i < wr->count && (i - app.scroll_y) < L.text_height; i++) {
        int screen_row = L.top_margin + (i - app.scroll_y);
        move_to(screen_row, L.margin + 1);
        WrapLine *line = &wr->lines[i];
        size_t p = line->start;
        int col = 0;
        
        while (p < line->end) {
            if (p == app.cursor) { cursor_screen_row = screen_row; cursor_screen_col = L.margin + 1 + col; }
            bool in_sel = app.selecting && p >= sel_s && p < sel_e;
            if (in_sel) set_bg(get_select());
            size_t next_pos;
            int w = grapheme_width_next(&app.text, p, &next_pos);
            for (size_t j = p; j < next_pos; j++) out_char(gap_at(&app.text, j));
            if (in_sel) set_bg(get_bg());
            col += w;
            p = next_pos;
        }
        if (app.cursor == line->end && i == cursor_vrow) {
            cursor_screen_row = screen_row; cursor_screen_col = L.margin + 1 + col;
        }
        if (line->ends_with_split) { set_fg(get_dim()); out_char('-'); set_fg(get_fg()); }
    }
    
    if (app.cursor >= len && wr->count > 0 && cursor_vrow <= app.scroll_y + L.text_height - 1) {
        cursor_screen_row = L.top_margin + (cursor_vrow - app.scroll_y);
        cursor_screen_col = L.margin + 1 + cursor_col_in_line;
    }
    
    move_to(cursor_screen_row, cursor_screen_col);
    cursor_visible(true);
}

//! Main render dispatch
static void render(void) {
    static AppMode last_mode = (AppMode)-1;
    if (app.mode != last_mode) {
        update_title();
        last_mode = app.mode;
    }

    sync_begin();
    cursor_visible(false);

    switch (app.mode) {
        case MODE_WELCOME: render_welcome(); break;
        case MODE_TIMER_SELECT: render_timer_select(); break;
        case MODE_STYLE: render_style_select(); break;
        case MODE_HISTORY: render_history(); break;
        case MODE_WRITING: render_writing(); break;
        case MODE_FINISHED: render_finished(); break;
        case MODE_TITLE_EDIT:
            if (app.prev_mode == MODE_WRITING) render_writing();
            else render_clear();
            render_title_edit();
            break;
        case MODE_HELP:
            render_writing();
            render_help();
            break;
        case MODE_IMAGE_EDIT:
            render_writing();
            render_image_edit();
            break;
        case MODE_TOC:
            render_writing();
            render_toc();
            break;
        case MODE_SEARCH:
            render_writing();
            render_search();
            break;
    }

    sync_end();
    out_flush();
}

// #endregion

// #region Session Management

static void new_session(void) {
    gap_free(&app.text);
    gap_init(&app.text, 4096);
    free(app.session_path);
    app.session_path = NULL;
    free(app.session_title);
    app.session_title = NULL;
    app.cursor = 0;
    app.selecting = false;
    app.timer_done = false;
    app.timer_on = (app.timer_mins > 0);
    if (app.timer_on) {
        const PlatformBackend *p = platform_get();
        app.timer_start = p && p->time_now ? p->time_now() : 0;
    }
    app.mode = MODE_WRITING;
    app.ai_open = false;
    app.ai_input_len = 0;
    app.ai_input_cursor = 0;
    chat_clear();
    
    #if HAS_LIBAI
    if (app.ai_ready && !app.ai_session) ai_init_session();
    #endif
}

// #endregion

// #region Input Handlers

//! Move cursor with optional selection extension
static void move_cursor(size_t new_pos, bool extend_sel) {
    if (extend_sel) {
        if (!app.selecting) {
            app.selecting = true;
            app.sel_anchor = app.cursor;
        }
    } else {
        app.selecting = false;
    }
    app.cursor = new_pos;
}

static void handle_writing(int key) {
    size_t len = gap_len(&app.text);
    
    switch (key) {
        case '\x1b':
            if (app.ai_open) app.ai_open = false;
            else if (app.preview_mode) app.quit = true;
            else { save_session(); app.mode = app.timer_on ? MODE_FINISHED : MODE_WELCOME; }
            break;
        case 16: timer_toggle_pause(); break;
        case 20: timer_add_minutes(5); break;
        case 6: if (!app.preview_mode) app.focus_mode = !app.focus_mode; break;
        case 18: app.plain_mode = !app.plain_mode; if (app.plain_mode) image_clear_all(); break;
        case 2: app.hide_cursor_syntax = !app.hide_cursor_syntax; break;
        case 14: footnote_jump(&app.text, &app.cursor); break;
        case 15: MODE_PUSH(MODE_HELP); break;

        case 12: // Ctrl+L - Table of Contents
            {
                if (!app.toc_state) {
                    app.toc_state = malloc(sizeof(TocState));
                    toc_init((TocState *)app.toc_state);
                }
                TocState *toc = (TocState *)app.toc_state;
                toc->filter_len = 0;
                toc->filter[0] = '\0';
                toc->selected = 0;
                toc->scroll = 0;
                toc_build(&app.text, toc);
                MODE_PUSH(MODE_TOC);
            }
            break;

        case 19: // Ctrl+S - Search
            {
                if (!app.search_state) {
                    app.search_state = malloc(sizeof(SearchState));
                    search_init((SearchState *)app.search_state);
                }
                SearchState *search = (SearchState *)app.search_state;
                search->selected = 0;
                search->scroll = 0;
                // Keep previous query for convenience
                search_find(&app.text, search);
                MODE_PUSH(MODE_SEARCH);
            }
            break;
        
        case 5:
            if (!CAN_MODIFY()) break;
            {
                size_t scan_start = app.cursor > 100 ? app.cursor - 100 : 0;
                for (size_t p = scan_start; p <= app.cursor && p < len; p++) {
                    size_t img_alt_s, img_alt_l, img_path_s, img_path_l, img_total;
                    int img_w, img_h;
                    if (md_check_image(&app.text, p, &img_alt_s, &img_alt_l, &img_path_s, &img_path_l,
                                      &img_w, &img_h, &img_total)) {
                        if (app.cursor >= p && app.cursor < p + img_total) {
                            app.img_edit_pos = p;
                            app.img_edit_total_len = img_total;
                            app.img_edit_field = 0;
                            app.img_edit_width_len = 0;
                            app.img_edit_height_len = 0;
                            app.img_edit_width_pct = (img_w < 0);
                            app.img_edit_height_pct = (img_h < 0);
                            if (img_w != 0) {
                                int val = img_w < 0 ? -img_w : img_w;
                                app.img_edit_width_len = (size_t)snprintf(app.img_edit_width_buf,
                                    sizeof(app.img_edit_width_buf), "%d", val);
                            }
                            if (img_h != 0) {
                                int val = img_h < 0 ? -img_h : img_h;
                                app.img_edit_height_len = (size_t)snprintf(app.img_edit_height_buf,
                                    sizeof(app.img_edit_height_buf), "%d", val);
                            }
                            MODE_PUSH(MODE_IMAGE_EDIT);
                            break;
                        }
                    }
                }
            }
            break;
        
        case 7:
            if (!CAN_MODIFY()) break;
            app.title_edit_len = 0;
            app.title_edit_cursor = 0;
            if (app.session_title) {
                size_t tlen = strlen(app.session_title);
                if (tlen >= sizeof(app.title_edit_buf)) tlen = sizeof(app.title_edit_buf) - 1;
                memcpy(app.title_edit_buf, app.session_title, tlen);
                app.title_edit_len = tlen;
                app.title_edit_cursor = tlen;
            }
            MODE_PUSH(MODE_TITLE_EDIT);
            break;
        
        case 26: if (CAN_MODIFY()) undo(); break;
        case 25: if (CAN_MODIFY()) redo(); break;
        
        case 31:
            #if HAS_LIBAI
            if (app.ai_ready && CAN_MODIFY()) {
                app.ai_open = !app.ai_open;
                app.ai_focused = app.ai_open;
                if (app.ai_open && !app.ai_session) ai_init_session();
            }
            #endif
            break;
        
        case KEY_LEFT: move_cursor(gap_utf8_prev(&app.text, app.cursor), false); break;
        case KEY_RIGHT: move_cursor(gap_utf8_next(&app.text, app.cursor), false); break;
        case KEY_UP: move_cursor(nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax), false); break;
        case KEY_DOWN: move_cursor(nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax), false); break;
        case KEY_ALT_LEFT: case KEY_CTRL_LEFT: move_cursor(nav_word_left(app.cursor), false); break;
        case KEY_ALT_RIGHT: case KEY_CTRL_RIGHT: move_cursor(nav_word_right(app.cursor), false); break;
        case KEY_SHIFT_LEFT: move_cursor(gap_utf8_prev(&app.text, app.cursor), true); break;
        case KEY_SHIFT_RIGHT: move_cursor(gap_utf8_next(&app.text, app.cursor), true); break;
        case KEY_SHIFT_UP: move_cursor(nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax), true); break;
        case KEY_SHIFT_DOWN: move_cursor(nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax), true); break;
        case KEY_CTRL_SHIFT_LEFT: case KEY_ALT_SHIFT_LEFT: move_cursor(nav_word_left(app.cursor), true); break;
        case KEY_CTRL_SHIFT_RIGHT: case KEY_ALT_SHIFT_RIGHT: move_cursor(nav_word_right(app.cursor), true); break;
        case KEY_HOME: move_cursor(nav_line_start(app.cursor), false); break;
        case KEY_END: move_cursor(nav_line_end(app.cursor), false); break;

        // Ctrl+Home/End: Jump to document start/end
        case KEY_CTRL_HOME: move_cursor(0, false); break;
        case KEY_CTRL_END: move_cursor(gap_len(&app.text), false); break;

        // Alt+Up/Down: Jump by half-screen
        case KEY_ALT_UP: {
            Layout L = calc_layout();
            int count = L.text_height / 2;
            if (count < 1) count = 1;
            for (int i = 0; i < count; i++) {
                size_t new_pos = nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax);
                if (new_pos == app.cursor) break;
                app.cursor = new_pos;
            }
            app.selecting = false;
            break;
        }
        case KEY_ALT_DOWN: {
            Layout L = calc_layout();
            int count = L.text_height / 2;
            if (count < 1) count = 1;
            for (int i = 0; i < count; i++) {
                size_t new_pos = nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax);
                if (new_pos == app.cursor) break;
                app.cursor = new_pos;
            }
            app.selecting = false;
            break;
        }

        // Mouse scroll: scroll view without moving cursor
        case KEY_MOUSE_SCROLL_UP: {
            app.scroll_y -= 3;
            if (app.scroll_y < 0) app.scroll_y = 0;
            break;
        }
        case KEY_MOUSE_SCROLL_DOWN: {
            app.scroll_y += 3;
            // Will be clamped during render
            break;
        }

        // Page Up/Down: scroll by half screen and move cursor
        case KEY_PGUP: {
            Layout L = calc_layout();
            int count = L.text_height / 2;
            if (count < 1) count = 1;
            for (int i = 0; i < count; i++) {
                size_t new_pos = nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax);
                if (new_pos == app.cursor) break;
                app.cursor = new_pos;
            }
            app.selecting = false;
            break;
        }
        case KEY_PGDN: {
            Layout L = calc_layout();
            int count = L.text_height / 2;
            if (count < 1) count = 1;
            for (int i = 0; i < count; i++) {
                size_t new_pos = nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax);
                if (new_pos == app.cursor) break;
                app.cursor = new_pos;
            }
            app.selecting = false;
            break;
        }
        
        case 1:
            app.sel_anchor = 0;
            app.cursor = gap_len(&app.text);
            app.selecting = true;
            break;
        
        case 3:
            if (has_selection()) {
                size_t s, e;
                get_selection(&s, &e);
                char *sel_text = gap_substr(&app.text, s, e);
                clipboard_copy(sel_text, e - s);
                free(sel_text);
            }
            break;
        
        case 22:
            if (!CAN_MODIFY()) break;
            {
                size_t paste_len;
                char *paste_text = clipboard_paste(&paste_len);
                if (paste_text && paste_len > 0) {
                    paste_len = normalize_line_endings(paste_text, paste_len);
                    save_undo_state();
                    delete_selection_if_any();
                    gap_insert_str(&app.text, app.cursor, paste_text, paste_len);
                    app.cursor += paste_len;
                }
                free(paste_text);
            }
            break;
        
        case 24:
            if (has_selection()) {
                size_t s, e;
                get_selection(&s, &e);
                char *sel_text = gap_substr(&app.text, s, e);
                clipboard_copy(sel_text, e - s);
                free(sel_text);
                if (CAN_EDIT()) {
                    save_undo_state();
                    delete_selection_if_any();
                }
            }
            break;
        
        case 127: case 8:
            if (!CAN_EDIT()) break;
            save_undo_state();
            delete_selection_if_any();
            if (!app.selecting && app.cursor > 0) {
                if (!smart_backspace()) {
                    size_t prev = gap_utf8_prev(&app.text, app.cursor);
                    gap_delete(&app.text, prev, app.cursor - prev);
                    app.cursor = prev;
                }
            }
            break;
        
        case KEY_DEL:
            if (!CAN_EDIT()) break;
            save_undo_state();
            delete_selection_if_any();
            if (!app.selecting && app.cursor < len) {
                size_t next = gap_utf8_next(&app.text, app.cursor);
                gap_delete(&app.text, app.cursor, next - app.cursor);
            }
            break;
        
        case 23:
            if (!CAN_EDIT()) break;
            save_undo_state();
            delete_selection_if_any();
            if (!app.selecting) {
                size_t new_pos = nav_word_left(app.cursor);
                gap_delete(&app.text, new_pos, app.cursor - new_pos);
                app.cursor = new_pos;
            }
            break;
        
        case 21:
            if (!CAN_EDIT()) break;
            save_undo_state();
            {
                size_t ls = nav_line_start(app.cursor);
                gap_delete(&app.text, ls, app.cursor - ls);
                app.cursor = ls;
                app.selecting = false;
            }
            break;
        
        case 11:
            if (!CAN_EDIT()) break;
            save_undo_state();
            {
                size_t le = nav_line_end(app.cursor);
                gap_delete(&app.text, app.cursor, le - app.cursor);
                app.selecting = false;
            }
            break;
        
        case 4:
            if (!CAN_EDIT()) break;
            save_undo_state();
            {
                size_t del_start, del_len;
                if (md_find_element_at(&app.text, app.cursor, &del_start, &del_len)) {
                    gap_delete(&app.text, del_start, del_len);
                    app.cursor = del_start;
                    app.selecting = false;
                } else if (app.cursor < len) {
                    size_t next = gap_utf8_next(&app.text, app.cursor);
                    gap_delete(&app.text, app.cursor, next - app.cursor);
                }
            }
            break;
        
        case '\t': {
            if (!CAN_MODIFY()) break;
            size_t line_start = find_line_start(app.cursor);
            size_t list_content, task_content;
            int list_indent, task_indent;
            bool in_list = md_check_list(&app.text, line_start, &list_content, &list_indent) > 0 ||
                          md_check_task(&app.text, line_start, &task_content, &task_indent) > 0;
            if (in_list) {
                gap_insert(&app.text, line_start, ' '); gap_insert(&app.text, line_start, ' ');
                app.cursor += 2;
            } else {
                gap_insert(&app.text, app.cursor, ' '); app.cursor++;
                gap_insert(&app.text, app.cursor, ' '); app.cursor++;
            }
            break;
        }
        
        case KEY_BTAB: {
            if (!CAN_MODIFY()) break;
            size_t line_start = find_line_start(app.cursor);
            int spaces = 0;
            while (line_start + spaces < gap_len(&app.text) &&
                   gap_at(&app.text, line_start + spaces) == ' ' && spaces < 2) spaces++;
            if (spaces > 0) {
                gap_delete(&app.text, line_start, spaces);
                app.cursor = (app.cursor >= line_start + spaces) ? app.cursor - spaces :
                             (app.cursor > line_start) ? line_start : app.cursor;
            }
            break;
        }
        
        case '\r': case '\n': {
            if (!CAN_MODIFY()) break;
            save_undo_state();
            delete_selection_if_any();
            
            size_t line_start = find_line_start(app.cursor);
            
            size_t task_content;
            int task_indent;
            if (md_check_task(&app.text, line_start, &task_content, &task_indent) > 0) {
                if (is_item_content_empty(&app.text, app.cursor, task_content)) {
                    handle_empty_list_item(&app.text, &app.cursor, line_start);
                } else {
                    gap_insert(&app.text, app.cursor++, '\n');
                    insert_chars_at_cursor(&app.text, &app.cursor, ' ', task_indent);
                    insert_str_at_cursor(&app.text, &app.cursor, "- [ ] ");
                }
                break;
            }
            
            size_t list_content;
            int list_indent;
            int list_type = md_check_list(&app.text, line_start, &list_content, &list_indent);
            if (list_type > 0) {
                if (is_item_content_empty(&app.text, app.cursor, list_content)) {
                    handle_empty_list_item(&app.text, &app.cursor, line_start);
                } else {
                    gap_insert(&app.text, app.cursor++, '\n');
                    insert_chars_at_cursor(&app.text, &app.cursor, ' ', list_indent);
                    if (list_type == 1) {
                        char marker[3] = { gap_at(&app.text, line_start + list_indent), ' ', '\0' };
                        insert_str_at_cursor(&app.text, &app.cursor, marker);
                    } else {
                        size_t p = line_start + list_indent;
                        int num = 0;
                        while (p < gap_len(&app.text) && gap_at(&app.text, p) >= '0' && gap_at(&app.text, p) <= '9')
                            num = num * 10 + (gap_at(&app.text, p++) - '0');
                        char num_buf[16];
                        snprintf(num_buf, sizeof(num_buf), "%d. ", num + 1);
                        insert_str_at_cursor(&app.text, &app.cursor, num_buf);
                    }
                }
                break;
            }
            
            size_t quote_content;
            int quote_level = md_check_blockquote(&app.text, line_start, &quote_content);
            if (quote_level > 0) {
                if (is_item_content_empty(&app.text, app.cursor, quote_content)) {
                    handle_empty_list_item(&app.text, &app.cursor, line_start);
                } else {
                    gap_insert(&app.text, app.cursor++, '\n');
                    for (int i = 0; i < quote_level; i++)
                        insert_str_at_cursor(&app.text, &app.cursor, "> ");
                }
                break;
            }
            
            gap_insert(&app.text, app.cursor++, '\n');
            break;
        }
        
        default:
            if (!CAN_MODIFY()) break;
            if (key >= 32 && key < 127) {
                save_undo_state();
                delete_selection_if_any();
                gap_insert(&app.text, app.cursor, (char)key);
                app.cursor++;
                check_auto_newline((char)key);
                if (key == ']') maybe_create_footnote_def(&app.text, app.cursor);
            }
            break;
    }
}

static void handle_ai_input(int key) {
    switch (key) {
        case '\x1b': app.ai_open = false; break;
        
        case '\r': case '\n':
            if (app.ai_input_len > 0 && !app.ai_thinking) {
                app.ai_input[app.ai_input_len] = '\0';
                #if HAS_LIBAI
                ai_send(app.ai_input);
                #endif
                app.ai_input_len = 0;
                app.ai_input_cursor = 0;
            }
            break;
        
        case 15:
            if (app.ai_input_len < MAX_AI_INPUT - 1) {
                memmove(app.ai_input + app.ai_input_cursor + 1,
                        app.ai_input + app.ai_input_cursor,
                        app.ai_input_len - app.ai_input_cursor);
                app.ai_input[app.ai_input_cursor] = '\n';
                app.ai_input_len++;
                app.ai_input_cursor++;
            }
            break;
        
        case 127: case 8:
            if (app.ai_input_cursor > 0) {
                memmove(app.ai_input + app.ai_input_cursor - 1,
                        app.ai_input + app.ai_input_cursor,
                        app.ai_input_len - app.ai_input_cursor);
                app.ai_input_len--;
                app.ai_input_cursor--;
            }
            break;
        
        case 22: {
            size_t paste_len;
            char *paste_text = clipboard_paste(&paste_len);
            if (paste_text && paste_len > 0) {
                if (app.ai_input_len + paste_len >= MAX_AI_INPUT)
                    paste_len = MAX_AI_INPUT - app.ai_input_len - 1;
                if (paste_len > 0) {
                    memmove(app.ai_input + app.ai_input_cursor + paste_len,
                            app.ai_input + app.ai_input_cursor,
                            app.ai_input_len - app.ai_input_cursor);
                    memcpy(app.ai_input + app.ai_input_cursor, paste_text, paste_len);
                    app.ai_input_len += paste_len;
                    app.ai_input_cursor += paste_len;
                }
            }
            free(paste_text);
            break;
        }
        
        case KEY_LEFT: if (app.ai_input_cursor > 0) app.ai_input_cursor--; break;
        case KEY_RIGHT: if (app.ai_input_cursor < app.ai_input_len) app.ai_input_cursor++; break;
        
        case KEY_UP: {
            size_t ls = app.ai_input_cursor;
            while (ls > 0 && app.ai_input[ls - 1] != '\n') ls--;
            size_t col = app.ai_input_cursor - ls;
            if (ls > 0) {
                size_t pe = ls - 1, ps = pe;
                while (ps > 0 && app.ai_input[ps - 1] != '\n') ps--;
                size_t pl = pe - ps;
                app.ai_input_cursor = ps + (col < pl ? col : pl);
            }
            break;
        }
        
        case KEY_DOWN: {
            size_t ls = app.ai_input_cursor;
            while (ls > 0 && app.ai_input[ls - 1] != '\n') ls--;
            size_t col = app.ai_input_cursor - ls;
            size_t le = app.ai_input_cursor;
            while (le < app.ai_input_len && app.ai_input[le] != '\n') le++;
            if (le < app.ai_input_len) {
                size_t ns = le + 1, ne = ns;
                while (ne < app.ai_input_len && app.ai_input[ne] != '\n') ne++;
                size_t nl = ne - ns;
                app.ai_input_cursor = ns + (col < nl ? col : nl);
            }
            break;
        }
        
        case KEY_HOME:
            while (app.ai_input_cursor > 0 && app.ai_input[app.ai_input_cursor - 1] != '\n')
                app.ai_input_cursor--;
            break;
        case KEY_END:
            while (app.ai_input_cursor < app.ai_input_len && app.ai_input[app.ai_input_cursor] != '\n')
                app.ai_input_cursor++;
            break;
        
        case KEY_PGUP: case KEY_MOUSE_SCROLL_UP:
            app.chat_scroll += (key == KEY_PGUP ? 10 : 3);
            break;
        case KEY_PGDN: case KEY_MOUSE_SCROLL_DOWN:
            app.chat_scroll -= (key == KEY_PGDN ? 10 : 3);
            if (app.chat_scroll < 0) app.chat_scroll = 0;
            break;
        
        default:
            if (key >= 32 && key < 127 && app.ai_input_len < MAX_AI_INPUT - 1) {
                memmove(app.ai_input + app.ai_input_cursor + 1,
                        app.ai_input + app.ai_input_cursor,
                        app.ai_input_len - app.ai_input_cursor);
                app.ai_input[app.ai_input_cursor] = (char)key;
                app.ai_input_len++;
                app.ai_input_cursor++;
            }
            break;
    }
}

static void handle_input(void) {
    int key = input_read_key();
    if (key == KEY_NONE) return;
    
    switch (app.mode) {
        case MODE_WELCOME:
            switch (key) {
                case 'q': app.quit = 1; break;
                case '\r': case '\n': new_session(); break;
                case 't': load_history(); app.mode = MODE_TIMER_SELECT; break;
                case 'h': load_history(); app.mode = MODE_HISTORY; break;
                case 'd':
                    app.theme = (app.theme == THEME_DARK) ? THEME_LIGHT : THEME_DARK;
                    highlight_cleanup(app.hl_ctx);
                    app.hl_ctx = highlight_init(app.theme == THEME_DARK);
                    break;
                case '?': MODE_PUSH(MODE_HELP); break;
            }
            break;
        
        case MODE_TIMER_SELECT:
            switch (key) {
                case '\x1b': app.mode = MODE_WELCOME; break;
                case 'k': case KEY_UP:
                    if (app.preset_idx > 0) app.preset_idx--;
                    app.timer_mins = TIMER_PRESETS[app.preset_idx];
                    break;
                case 'j': case KEY_DOWN:
                    if (app.preset_idx < (int)NUM_PRESETS - 1) app.preset_idx++;
                    app.timer_mins = TIMER_PRESETS[app.preset_idx];
                    break;
                case '\r': case '\n': app.mode = MODE_WELCOME; break;
            }
            break;
        
        case MODE_STYLE:
            switch (key) {
                case '\x1b': app.mode = MODE_WELCOME; break;
                case 'k': case KEY_UP: if (app.style > STYLE_MINIMAL) app.style--; break;
                case 'j': case KEY_DOWN: if (app.style < STYLE_ELEGANT) app.style++; break;
                case '\r': case '\n': app.mode = MODE_WELCOME; break;
            }
            break;
        
        case MODE_HISTORY:
            switch (key) {
                case '\x1b': app.mode = MODE_WELCOME; break;
                case 'k': case KEY_UP: if (app.hist_sel > 0) app.hist_sel--; break;
                case 'j': case KEY_DOWN: if (app.hist_sel < app.hist_count - 1) app.hist_sel++; break;
                case 'o': case '\r': case '\n':
                    if (app.hist_count > 0) load_file_for_editing(app.history[app.hist_sel].path);
                    break;
                case 'e':
                    if (app.hist_count > 0) open_in_finder(app.history[app.hist_sel].path);
                    break;
                case 't':
                    if (app.hist_count > 0) {
                        load_file_for_editing(app.history[app.hist_sel].path);
                        app.title_edit_len = 0;
                        app.title_edit_cursor = 0;
                        if (app.session_title) {
                            size_t tlen = strlen(app.session_title);
                            if (tlen >= sizeof(app.title_edit_buf)) tlen = sizeof(app.title_edit_buf) - 1;
                            memcpy(app.title_edit_buf, app.session_title, tlen);
                            app.title_edit_len = tlen;
                            app.title_edit_cursor = tlen;
                        }
                        app.prev_mode = MODE_WRITING;
                        app.mode = MODE_TITLE_EDIT;
                    }
                    break;
                case 'd':
                    if (app.hist_count > 0) {
                        HistoryEntry *entry = &app.history[app.hist_sel];
                        remove(entry->path);
                        char chat_path[520];
                        get_chat_path(entry->path, chat_path, sizeof(chat_path));
                        remove(chat_path);
                        free(entry->path); free(entry->title); free(entry->date_str);
                        for (int i = app.hist_sel; i < app.hist_count - 1; i++)
                            app.history[i] = app.history[i + 1];
                        app.hist_count--;
                        if (app.hist_sel >= app.hist_count && app.hist_sel > 0) app.hist_sel--;
                        if (app.hist_count == 0) app.mode = MODE_WELCOME;
                    }
                    break;
            }
            break;
        
        case MODE_WRITING:
            if (app.ai_open && key == '\t') { app.ai_focused = !app.ai_focused; break; }
            if (app.ai_open && app.ai_focused) handle_ai_input(key);
            else handle_writing(key);
            break;
        
        case MODE_FINISHED:
            switch (key) {
                case 'q': app.quit = 1; break;
                case '\x1b': app.mode = MODE_WELCOME; break;
                case '\r': case '\n': new_session(); break;
                case 'o': if (app.session_path) open_in_finder(app.session_path); break;
                case 'c': app.mode = MODE_WRITING; app.timer_on = false; break;
                case '/': case 31:
                    #if HAS_LIBAI
                    if (app.ai_ready) {
                        app.mode = MODE_WRITING;
                        app.ai_open = true;
                        app.ai_focused = true;
                        if (!app.ai_session) ai_init_session();
                    }
                    #endif
                    break;
            }
            break;
        
        case MODE_TITLE_EDIT:
            switch (key) {
                case '\x1b': MODE_POP(); break;
                case '\r': case '\n':
                    free(app.session_title);
                    if (app.title_edit_len > 0) {
                        app.session_title = malloc(app.title_edit_len + 1);
                        if (app.session_title) {
                            memcpy(app.session_title, app.title_edit_buf, app.title_edit_len);
                            app.session_title[app.title_edit_len] = '\0';
                        }
                    } else {
                        app.session_title = NULL;
                    }
                    save_session();
                    update_title();
                    MODE_POP();
                    break;
                case 127: case '\b':
                    if (app.title_edit_cursor > 0) {
                        memmove(app.title_edit_buf + app.title_edit_cursor - 1,
                                app.title_edit_buf + app.title_edit_cursor,
                                app.title_edit_len - app.title_edit_cursor);
                        app.title_edit_cursor--;
                        app.title_edit_len--;
                    }
                    break;
                case KEY_DEL:
                    if (app.title_edit_cursor < app.title_edit_len) {
                        memmove(app.title_edit_buf + app.title_edit_cursor,
                                app.title_edit_buf + app.title_edit_cursor + 1,
                                app.title_edit_len - app.title_edit_cursor - 1);
                        app.title_edit_len--;
                    }
                    break;
                case KEY_LEFT: if (app.title_edit_cursor > 0) app.title_edit_cursor--; break;
                case KEY_RIGHT: if (app.title_edit_cursor < app.title_edit_len) app.title_edit_cursor++; break;
                case KEY_HOME: app.title_edit_cursor = 0; break;
                case KEY_END: app.title_edit_cursor = app.title_edit_len; break;
                default:
                    if (key >= 32 && key < 127 && app.title_edit_len < sizeof(app.title_edit_buf) - 1) {
                        memmove(app.title_edit_buf + app.title_edit_cursor + 1,
                                app.title_edit_buf + app.title_edit_cursor,
                                app.title_edit_len - app.title_edit_cursor);
                        app.title_edit_buf[app.title_edit_cursor] = (char)key;
                        app.title_edit_cursor++;
                        app.title_edit_len++;
                    }
                    break;
            }
            break;
        
        case MODE_IMAGE_EDIT:
            switch (key) {
                case '\x1b': MODE_POP(); break;
                case '\r': case '\n': {
                    char new_syntax[2048];
                    size_t alt_s, alt_l, path_s, path_l, total;
                    int old_w, old_h;
                    if (md_check_image(&app.text, app.img_edit_pos, &alt_s, &alt_l,
                                      &path_s, &path_l, &old_w, &old_h, &total)) {
                        char alt[512], path[1024];
                        for (size_t i = 0; i < alt_l && i < sizeof(alt) - 1; i++)
                            alt[i] = gap_at(&app.text, alt_s + i);
                        alt[alt_l < sizeof(alt) ? alt_l : sizeof(alt) - 1] = '\0';
                        for (size_t i = 0; i < path_l && i < sizeof(path) - 1; i++)
                            path[i] = gap_at(&app.text, path_s + i);
                        path[path_l < sizeof(path) ? path_l : sizeof(path) - 1] = '\0';
                        
                        int w_val = 0, h_val = 0;
                        if (app.img_edit_width_len > 0) {
                            char w_str[16];
                            memcpy(w_str, app.img_edit_width_buf, app.img_edit_width_len);
                            w_str[app.img_edit_width_len] = '\0';
                            w_val = atoi(w_str);
                            if (app.img_edit_width_pct) w_val = -w_val;
                        }
                        if (app.img_edit_height_len > 0) {
                            char h_str[16];
                            memcpy(h_str, app.img_edit_height_buf, app.img_edit_height_len);
                            h_str[app.img_edit_height_len] = '\0';
                            h_val = atoi(h_str);
                            if (app.img_edit_height_pct) h_val = -h_val;
                        }
                        
                        int len = snprintf(new_syntax, sizeof(new_syntax), "![%s](%s)", alt, path);
                        if (w_val != 0 || h_val != 0) {
                            len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "{ ");
                            if (w_val != 0) {
                                if (w_val < 0)
                                    len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "width=%d%%", -w_val);
                                else
                                    len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "width=%dpx", w_val);
                            }
                            if (h_val != 0) {
                                if (w_val != 0) len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, " ");
                                if (h_val < 0)
                                    len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "height=%d%%", -h_val);
                                else
                                    len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "height=%dpx", h_val);
                            }
                            len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, " }");
                        }
                        
                        gap_delete(&app.text, app.img_edit_pos, app.img_edit_total_len);
                        gap_insert_str(&app.text, app.img_edit_pos, new_syntax, (size_t)len);
                        app.cursor = app.img_edit_pos;
                    }
                    MODE_POP();
                    break;
                }
                case '\t': app.img_edit_field = (app.img_edit_field + 1) % 2; break;
                case 'p': case 'P':
                    if (app.img_edit_field == 0) app.img_edit_width_pct = !app.img_edit_width_pct;
                    else app.img_edit_height_pct = !app.img_edit_height_pct;
                    break;
                case 127: case '\b':
                    if (app.img_edit_field == 0 && app.img_edit_width_len > 0) app.img_edit_width_len--;
                    else if (app.img_edit_field == 1 && app.img_edit_height_len > 0) app.img_edit_height_len--;
                    break;
                default:
                    if (key >= '0' && key <= '9') {
                        if (app.img_edit_field == 0 && app.img_edit_width_len < sizeof(app.img_edit_width_buf) - 1)
                            app.img_edit_width_buf[app.img_edit_width_len++] = (char)key;
                        else if (app.img_edit_field == 1 && app.img_edit_height_len < sizeof(app.img_edit_height_buf) - 1)
                            app.img_edit_height_buf[app.img_edit_height_len++] = (char)key;
                    }
                    break;
            }
            break;
        
        case MODE_HELP: MODE_POP(); break;

        case MODE_TOC:
            {
                TocState *toc = (TocState *)app.toc_state;
                if (!toc) { MODE_POP(); break; }

                switch (key) {
                    case '\x1b':
                        MODE_POP();
                        break;
                    case '\r': case '\n':
                        {
                            const TocEntry *entry = toc_get_selected(toc);
                            if (entry) {
                                app.cursor = entry->pos;
                                app.selecting = false;
                            }
                            clear_screen();
                            MODE_POP();
                        }
                        break;
                    case KEY_UP: case 'k':
                        if (toc->selected > 0) toc->selected--;
                        break;
                    case KEY_DOWN: case 'j':
                        if (toc->selected < toc->filtered_count - 1) toc->selected++;
                        break;
                    case KEY_PGUP:
                        toc->selected -= 10;
                        if (toc->selected < 0) toc->selected = 0;
                        break;
                    case KEY_PGDN:
                        toc->selected += 10;
                        if (toc->selected >= toc->filtered_count) toc->selected = toc->filtered_count - 1;
                        if (toc->selected < 0) toc->selected = 0;
                        break;
                    case 127: case '\b':
                        if (toc->filter_len > 0) {
                            toc->filter_len--;
                            toc->filter[toc->filter_len] = '\0';
                            toc_filter(toc);
                        }
                        break;
                    default:
                        if (key >= 32 && key < 127 && toc->filter_len < (int)sizeof(toc->filter) - 1) {
                            toc->filter[toc->filter_len++] = (char)key;
                            toc->filter[toc->filter_len] = '\0';
                            toc_filter(toc);
                        }
                        break;
                }
            }
            break;

        case MODE_SEARCH:
            {
                SearchState *search = (SearchState *)app.search_state;
                if (!search) { MODE_POP(); break; }

                switch (key) {
                    case '\x1b':
                        MODE_POP();
                        break;
                    case '\r': case '\n':
                        {
                            const SearchResult *r = search_get_selected(search);
                            if (r) {
                                app.cursor = r->pos;
                                app.selecting = false;
                            }
                            clear_screen();
                            MODE_POP();
                        }
                        break;
                    case KEY_UP: case 16: // Up or Ctrl+P
                        if (search->selected > 0) search->selected--;
                        break;
                    case KEY_DOWN: case 14: // Down or Ctrl+N
                        if (search->selected < search->count - 1) search->selected++;
                        break;
                    case KEY_PGUP:
                        search->selected -= 10;
                        if (search->selected < 0) search->selected = 0;
                        break;
                    case KEY_PGDN:
                        search->selected += 10;
                        if (search->selected >= search->count) search->selected = search->count - 1;
                        if (search->selected < 0) search->selected = 0;
                        break;
                    case 127: case '\b':
                        if (search->query_len > 0) {
                            search->query_len--;
                            search->query[search->query_len] = '\0';
                            search_find(&app.text, search);
                        }
                        break;
                    default:
                        if (key >= 32 && key < 127 && search->query_len < SEARCH_MAX_QUERY - 1) {
                            search->query[search->query_len++] = (char)key;
                            search->query[search->query_len] = '\0';
                            search_find(&app.text, search);
                        }
                        break;
                }
            }
            break;
    }
}

// #endregion

// #region Engine API

bool dawn_engine_init(Theme theme) {
    app.timer_mins = DEFAULT_TIMER_MINUTES;
    app.mode = MODE_WELCOME;
    app.theme = theme;
    app.style = STYLE_MINIMAL;
    
    for (size_t i = 0; i < NUM_PRESETS; i++) {
        if (TIMER_PRESETS[i] == DEFAULT_TIMER_MINUTES) {
            app.preset_idx = (int)i;
            break;
        }
    }
    
    gap_init(&app.text, 4096);
    
    app.block_cache = malloc(sizeof(BlockCache));
    if (app.block_cache) block_cache_init((BlockCache *)app.block_cache);
    
    app.hl_ctx = highlight_init(theme == THEME_DARK);
    dawn_update_size();
    
    #if HAS_LIBAI
    search_tool_init();
    ai_result_t init_result = ai_init();
    if (init_result == AI_SUCCESS) {
        ai_availability_t status = ai_check_availability();
        if (status == AI_AVAILABLE) {
            app.ai_ctx = ai_context_create();
            if (app.ai_ctx) app.ai_ready = true;
        }
    }
    #endif
    
    return true;
}

void dawn_engine_shutdown(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_title) p->set_title(NULL);

    if (gap_len(&app.text) > 0 && app.mode == MODE_WRITING && !app.preview_mode)
        save_session();
    
    gap_free(&app.text);
    free(app.session_path); app.session_path = NULL;
    free(app.session_title); app.session_title = NULL;
    chat_clear();
    
    if (app.history) {
        for (int i = 0; i < app.hist_count; i++) {
            free(app.history[i].path);
            free(app.history[i].title);
            free(app.history[i].date_str);
        }
        free(app.history);
        app.history = NULL;
    }
    
    for (int i = 0; i < app.undo_count; i++) free(app.undo_stack[i].text);
    app.undo_count = 0;
    
    if (app.block_cache) {
        block_cache_free((BlockCache *)app.block_cache);
        free(app.block_cache);
        app.block_cache = NULL;
    }
    
    #if HAS_LIBAI
    if (app.ai_ctx && app.ai_session) ai_destroy_session(app.ai_ctx, app.ai_session);
    if (app.ai_ctx) ai_context_free(app.ai_ctx);
    ai_cleanup();
    search_tool_cleanup();
    #endif
}

bool dawn_frame(void) {
    const PlatformBackend *p = platform_get();

    if (app.quit) return false;
    if (p && p->check_quit && p->check_quit()) return false;
    if (p && p->check_resize && p->check_resize()) dawn_update_size();
    if (app.timer_on) timer_check();
    
    if (app.mode == MODE_WRITING && gap_len(&app.text) > 0 && !app.preview_mode) {
        int64_t now = p && p->time_now ? p->time_now() : 0;
        if (app.last_save_time == 0) app.last_save_time = now;
        else if (now - app.last_save_time >= 60) {
            save_session();
            app.last_save_time = now;
        }
    }
    
    render();
    handle_input();
    return true;
}

void dawn_request_quit(void) { app.quit = true; }
bool dawn_should_quit(void) { return app.quit; }

bool dawn_load_document(const char *path) {
    load_file_for_editing(path);
    return true;
}

bool dawn_preview_document(const char *path) {
    load_file_for_editing(path);
    app.preview_mode = true;
    app.mode = MODE_WRITING;
    app.timer_on = false;
    app.timer_mins = 0;
    return true;
}

void dawn_new_document(void) { new_session(); }

void dawn_save_document(void) { save_session(); }

void dawn_update_size(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->get_size) p->get_size(&app.cols, &app.rows);
}

void dawn_render(void) { render(); }

// #endregion

// #endregion

// #region Main Writing Renderer

// Forward declarations for block rendering
static void render_block(const RenderCtx *ctx, RenderState *rs, const Block *block);

static void render_writing(void) {
    if (app.plain_mode) { render_writing_plain(); return; }

    Layout L = calc_layout();
    image_frame_start();
    set_bg(get_bg());
    cursor_home();

    // Clear screen
    for (int r = 0; r < app.rows; r++) {
        move_to(r + 1, 1);
        set_bg(get_bg());
        for (int c = 0; c < L.text_area_cols; c++) out_char(' ');
        if (app.ai_open) {
            set_bg(get_bg());
            set_fg(get_border());
            out_char(' ');
            set_bg(get_ai_bg());
            for (int c = 0; c < L.ai_cols; c++) out_char(' ');
        }
    }

    if (app.style == STYLE_ELEGANT) set_italic(true);
    if (app.ai_open && app.ai_focused) set_dim(true);

    size_t len = gap_len(&app.text);
    int max_row = L.top_margin + L.text_height;

    // Ensure block cache is valid
    BlockCache *bc = (BlockCache *)app.block_cache;
    if (!bc) {
        // Fallback: allocate if not present
        app.block_cache = malloc(sizeof(BlockCache));
        bc = (BlockCache *)app.block_cache;
        if (bc) block_cache_init(bc);
    }

    if (bc && (!bc->valid || bc->text_len != len || bc->wrap_width != L.text_width || bc->text_height != L.text_height)) {
        block_cache_parse(bc, &app.text, L.text_width, L.text_height);
    }

    // Calculate cursor virtual row using block cache
    int cursor_vrow = 0;
    if (bc && bc->valid && bc->count > 0) {
        if (app.cursor >= len) {
            // Cursor at end of document - position after last block
            Block *last_block = &bc->blocks[bc->count - 1];
            cursor_vrow = last_block->vrow_start + last_block->vrow_count;
        } else {
            int cursor_block_idx = block_index_at_pos(bc, app.cursor);
            if (cursor_block_idx >= 0) {
                Block *cursor_block = &bc->blocks[cursor_block_idx];
                cursor_vrow = cursor_block->vrow_start +
                              calc_cursor_vrow_in_block(cursor_block, &app.text, app.cursor, L.text_width);
            }
        }
    }

    // Adjust scroll with margin to keep cursor away from edges
    int scroll_margin = L.text_height > 10 ? 3 : 1;
    if (cursor_vrow < app.scroll_y + scroll_margin) {
        app.scroll_y = cursor_vrow - scroll_margin;
    } else if (cursor_vrow >= app.scroll_y + L.text_height - scroll_margin) {
        app.scroll_y = cursor_vrow - L.text_height + scroll_margin + 1;
    }
    if (app.scroll_y < 0) app.scroll_y = 0;

    // Initialize render state
    RenderState rs = {0};
    rs.cursor_virtual_row = cursor_vrow;
    rs.cursor_col = L.margin + 1;

    RenderCtx ctx = { .L = L, .max_row = max_row, .len = len,
                      .cursor_virtual_row = &rs.cursor_virtual_row,
                      .cursor_col = &rs.cursor_col };

    // Find first visible block using binary search
    int start_block_idx = 0;
    if (bc && bc->valid && bc->count > 0 && app.scroll_y > 0) {
        Block *start_block = block_at_vrow(bc, app.scroll_y);
        if (start_block) {
            start_block_idx = (int)(start_block - bc->blocks);
        }
    }

    // Render visible blocks
    // Track running vrow to handle cursor-in-block expanding beyond calculated vrows
    int running_vrow = 0;
    if (bc && bc->valid && start_block_idx > 0) {
        // Start from where previous blocks would have ended
        running_vrow = bc->blocks[start_block_idx].vrow_start;
    }

    if (bc && bc->valid) {
        for (int bi = start_block_idx; bi < bc->count; bi++) {
            Block *block = &bc->blocks[bi];

            // Use running_vrow instead of block->vrow_start to handle expansion
            int block_screen_start = vrow_to_screen(&L, running_vrow, app.scroll_y);
            if (block_screen_start > max_row) break;

            // Set render state for this block
            rs.pos = block->start;
            rs.virtual_row = running_vrow;
            rs.col_width = 0;
            rs.line_style = 0;
            rs.style_depth = 0;
            rs.active_style = 0;
            rs.in_block_math = false;

            // Render the block
            render_block(&ctx, &rs, block);

            // Update running_vrow from actual rendered rows
            running_vrow = rs.virtual_row;
        }
    }

    // Handle cursor at end of document
    if (app.cursor >= len) {
        // Use running_vrow which reflects actual rendered rows
        rs.cursor_virtual_row = running_vrow;
        rs.cursor_col = L.margin + 1 + rs.col_width;
    }

    // Re-adjust scroll based on actual rendered cursor position
    // This handles cases where cursor-in-block expands beyond cached vrows
    if (rs.cursor_virtual_row < app.scroll_y + scroll_margin) {
        app.scroll_y = rs.cursor_virtual_row - scroll_margin;
        if (app.scroll_y < 0) app.scroll_y = 0;
    } else if (rs.cursor_virtual_row >= app.scroll_y + L.text_height - scroll_margin) {
        app.scroll_y = rs.cursor_virtual_row - L.text_height + scroll_margin + 1;
    }

    reset_attrs();
    set_bg(get_bg());

    render_status_bar(&L);

    if (app.ai_open) {
        render_ai_panel(&L);
        if (app.ai_focused) {
            image_frame_end();
            out_flush();
            return;
        }
        reset_attrs();
    }

    image_frame_end();
    int cursor_screen_row = vrow_to_screen(&L, rs.cursor_virtual_row, app.scroll_y);
    if (cursor_screen_row < L.top_margin) cursor_screen_row = L.top_margin;
    if (cursor_screen_row > max_row) cursor_screen_row = max_row;
    if (rs.cursor_col < L.margin + 1) rs.cursor_col = L.margin + 1;
    move_to(cursor_screen_row, rs.cursor_col);
    cursor_visible(true);
}

//! Render a single block - dispatches to type-specific renderer
static void render_block(const RenderCtx *ctx, RenderState *rs, const Block *block) {
    bool cursor_in_block = (app.cursor >= block->start && app.cursor < block->end);

    switch (block->type) {
        case BLOCK_IMAGE:
            render_image_element(ctx, rs,
                block->data.image.alt_start, block->data.image.alt_len,
                block->data.image.path_start, block->data.image.path_len,
                block->data.image.width, block->data.image.height,
                block->end - block->start);
            break;

        case BLOCK_HR: {
            size_t hr_len = block->end - block->start;
            if (hr_len > 0 && gap_at(&app.text, block->end - 1) == '\n') hr_len--;
            render_hr_element(ctx, rs, hr_len);
            break;
        }

        case BLOCK_HEADER: {
            if (HAS_CAP(PLATFORM_CAP_TEXT_SIZING)) {
                size_t header_end = block->end;
                if (header_end > 0 && gap_at(&app.text, header_end - 1) == '\n') header_end--;
                MdStyle header_style = md_style_for_header_level(block->data.header.level);
                render_header_element(ctx, rs, block->data.header.content_start, header_end,
                                     block->data.header.level, header_style);
            } else {
                // Fall through to paragraph-style rendering for non-scalable platforms
                goto render_as_paragraph;
            }
            break;
        }

        case BLOCK_CODE:
            render_code_block_element(ctx, rs,
                block->data.code.lang_start, block->data.code.lang_len,
                block->data.code.content_start, block->data.code.content_len,
                block->end - block->start);
            break;

        case BLOCK_MATH:
            render_block_math_element(ctx, rs,
                block->data.math.content_start, block->data.math.content_len,
                block->end - block->start);
            break;

        case BLOCK_TABLE: {
            // Reparse table for rendering (we need the full MdTable struct)
            MdTable tbl;
            if (md_check_table(&app.text, block->start, &tbl)) {
                render_table_element(ctx, rs, &tbl);
            }
            break;
        }

        case BLOCK_BLOCKQUOTE:
        case BLOCK_LIST_ITEM:
        case BLOCK_FOOTNOTE_DEF:
        case BLOCK_PARAGRAPH:
        render_as_paragraph: {
            // Render paragraph-like blocks with inline markdown
            // This handles blockquotes, lists, footnotes, and paragraphs
            size_t len = ctx->len;
            size_t sel_s, sel_e;
            get_selection(&sel_s, &sel_e);

            rs->pos = block->start;
            // Note: rs->virtual_row is set by caller (render_writing) to running_vrow
            rs->col_width = 0;

            while (rs->pos < block->end && rs->pos < len) {
                int screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                char c = gap_at(&app.text, rs->pos);

                track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                            &rs->cursor_virtual_row, &rs->cursor_col);

                // Handle newline
                if (c == '\n') {
                    rs->pos++;
                    int newline_scale = get_line_scale(rs->line_style);
                    rs->virtual_row += newline_scale;
                    rs->col_width = 0;
                    rs->line_style = 0;
                    rs->style_depth = 0;
                    rs->active_style = 0;
                    reset_attrs();
                    set_bg(get_bg());
                    current_text_scale = 1;
                    current_frac_num = 0;
                    current_frac_denom = 0;
                    continue;
                }

                // Check line-level elements at line start
                bool at_line_start = (rs->pos == block->start || gap_at(&app.text, rs->pos - 1) == '\n');
                if (rs->col_width == 0 && at_line_start) {
                    // Set line style for headers (when not using text scaling)
                    if (!HAS_CAP(PLATFORM_CAP_TEXT_SIZING)) {
                        rs->line_style = md_check_header(&app.text, rs->pos);
                    }
                }

                // Find end of logical line within block
                size_t line_end = rs->pos;
                while (line_end < block->end && line_end < len && gap_at(&app.text, line_end) != '\n') {
                    line_end++;
                }

                // Calculate wrap segment
                int text_scale = get_line_scale(rs->line_style);
                int seg_width;
                int available_width = (ctx->L.text_width - rs->col_width) / text_scale;
                if (available_width < 1) available_width = 1;
                size_t seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, available_width, &seg_width);

                // Render line prefixes (blockquote bars, list bullets, etc.)
                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                    if (rs->col_width == 0) move_to(screen_row, ctx->L.margin + 1);
                    render_line_prefixes(ctx, rs, line_end, &seg_end, &seg_width);
                }

                // Render segment content with inline markdown
                while (rs->pos < seg_end && rs->pos < len) {
                    screen_row = vrow_to_screen(&ctx->L, rs->virtual_row, app.scroll_y);
                    if (screen_row > ctx->max_row) { rs->pos = seg_end; break; }

                    track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                                &rs->cursor_virtual_row, &rs->cursor_col);

                    bool in_sel = has_selection() && rs->pos >= sel_s && rs->pos < sel_e;

                    // Inline math
                    if (!rs->in_block_math) {
                        size_t math_cs, math_cl, math_total;
                        if (md_check_inline_math(&app.text, rs->pos, &math_cs, &math_cl, &math_total)) {
                            render_inline_math(ctx, rs, math_cs, math_cl, math_total);
                            continue;
                        }
                    }

                    // Link
                    size_t link_ts, link_tl, link_us, link_ul, link_total;
                    if (!rs->in_block_math && md_check_link(&app.text, rs->pos, &link_ts, &link_tl,
                                                           &link_us, &link_ul, &link_total)) {
                        render_link(ctx, rs, link_ts, link_tl, link_us, link_ul, link_total);
                        continue;
                    }

                    // Autolink (<https://...> or <email@domain>)
                    if (!rs->in_block_math && gap_at(&app.text, rs->pos) == '<') {
                        size_t auto_us, auto_ul, auto_total;
                        bool auto_is_email;
                        if (md_check_autolink(&app.text, rs->pos, &auto_us, &auto_ul, &auto_total, &auto_is_email)) {
                            // Render as a link - show the URL text with link styling
                            bool cursor_in_auto = cursor_in_range(app.cursor, rs->pos, rs->pos + auto_total, app.hide_cursor_syntax);
                            if (cursor_in_auto) {
                                // Show full syntax when cursor is inside
                                set_fg(get_dim());
                                for (size_t i = 0; i < auto_total && rs->pos < len; i++) {
                                    track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                                                &rs->cursor_virtual_row, &rs->cursor_col);
                                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                        rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                    else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                }
                                set_fg(get_fg());
                            } else {
                                // Render URL text with link styling (hide < and >)
                                set_fg(get_accent());
                                set_underline(UNDERLINE_STYLE_SINGLE);
                                rs->pos++;  // skip <
                                size_t url_end = rs->pos + auto_ul;
                                while (rs->pos < url_end && rs->pos < len) {
                                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                        rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                    else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                }
                                rs->pos++;  // skip >
                                set_underline(0);
                                set_fg(get_fg());
                            }
                            continue;
                        }
                    }

                    // Footnote ref
                    size_t fnref_is, fnref_il, fnref_total;
                    if (!rs->in_block_math && md_check_footnote_ref(&app.text, rs->pos, &fnref_is, &fnref_il, &fnref_total)) {
                        render_footnote_ref(ctx, rs, fnref_is, fnref_il, fnref_total);
                        continue;
                    }

                    // Heading ID
                    if ((rs->line_style & (MD_H1 | MD_H2 | MD_H3 | MD_H4 | MD_H5 | MD_H6)) && gap_at(&app.text, rs->pos) == '{') {
                        size_t hid_s, hid_l, hid_total;
                        if (md_check_heading_id(&app.text, rs->pos, &hid_s, &hid_l, &hid_total)) {
                            render_heading_id(ctx, rs, hid_s, hid_l, hid_total);
                            continue;
                        }
                    }

                    // Emoji
                    if (!rs->in_block_math && gap_at(&app.text, rs->pos) == ':') {
                        size_t emoji_sc_s, emoji_sc_l, emoji_total;
                        const char *emoji = md_check_emoji(&app.text, rs->pos, &emoji_sc_s, &emoji_sc_l, &emoji_total);
                        if (emoji) {
                            render_emoji(ctx, rs, emoji, emoji_total);
                            continue;
                        }
                    }

                    // HTML entity references (&nbsp; &#123; &#x1F;)
                    if (!rs->in_block_math && gap_at(&app.text, rs->pos) == '&') {
                        char entity_utf8[8];
                        size_t entity_total;
                        int entity_len = md_check_entity(&app.text, rs->pos, entity_utf8, &entity_total);
                        if (entity_len > 0) {
                            // Render the decoded character(s)
                            bool cursor_in_entity = cursor_in_range(app.cursor, rs->pos, rs->pos + entity_total, app.hide_cursor_syntax);
                            if (cursor_in_entity) {
                                // Show the raw entity when cursor is inside
                                set_fg(get_dim());
                                for (size_t i = 0; i < entity_total && rs->pos < len; i++) {
                                    track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                                                &rs->cursor_virtual_row, &rs->cursor_col);
                                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                        rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                    else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                }
                                set_fg(get_fg());
                            } else {
                                // Output decoded UTF-8 and skip source
                                if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                                    out_str_n(entity_utf8, entity_len);
                                    rs->col_width += utf8_display_width(entity_utf8, entity_len);
                                }
                                rs->pos += entity_total;
                            }
                            continue;
                        }
                    }

                    // Backslash escape - makes the backslash invisible and shows next char literally
                    // CommonMark: All ASCII punctuation can be escaped
                    if (!rs->in_block_math && gap_at(&app.text, rs->pos) == '\\' && rs->pos + 1 < len) {
                        char next_ch = gap_at(&app.text, rs->pos + 1);
                        // Per CommonMark spec, all ASCII punctuation chars are escapable:
                        // !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~
                        // Plus newline for hard line breaks
                        bool is_escapable = (next_ch == '!' || next_ch == '"' || next_ch == '#' ||
                            next_ch == '$' || next_ch == '%' || next_ch == '&' || next_ch == '\'' ||
                            next_ch == '(' || next_ch == ')' || next_ch == '*' || next_ch == '+' ||
                            next_ch == ',' || next_ch == '-' || next_ch == '.' || next_ch == '/' ||
                            next_ch == ':' || next_ch == ';' || next_ch == '<' || next_ch == '=' ||
                            next_ch == '>' || next_ch == '?' || next_ch == '@' || next_ch == '[' ||
                            next_ch == '\\' || next_ch == ']' || next_ch == '^' || next_ch == '_' ||
                            next_ch == '`' || next_ch == '{' || next_ch == '|' || next_ch == '}' ||
                            next_ch == '~' || next_ch == '\n');
                        if (is_escapable) {
                            bool cursor_on_backslash = cursor_in_range(app.cursor, rs->pos, rs->pos + 1, app.hide_cursor_syntax);
                            if (cursor_on_backslash) {
                                // Show the backslash when cursor is on it
                                set_fg(get_dim());
                                if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                    rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                set_fg(get_fg());
                            } else {
                                // Skip the backslash entirely
                                rs->pos++;
                                // Handle \<newline> as a hard line break (invisible)
                                if (next_ch == '\n') {
                                    // The newline will be processed normally on next iteration
                                    continue;
                                }
                            }
                            // The escaped character will be rendered normally on next iteration
                            continue;
                        }
                    }

                    // Markdown delimiters
                    size_t dlen = 0;
                    MdStyle delim = 0;
                    if (!rs->in_block_math) delim = md_check_delim(&app.text, rs->pos, &dlen);

                    if (delim != 0 && dlen > 0) {
                        int close_idx = -1;
                        for (int si = rs->style_depth - 1; si >= 0; si--) {
                            if (delim == rs->style_stack[si].style && dlen == rs->style_stack[si].dlen &&
                                rs->pos == rs->style_stack[si].close_pos) {
                                close_idx = si;
                                break;
                            }
                        }

                        if (close_idx >= 0) {
                            bool cursor_in_delim = cursor_in_range(app.cursor, rs->pos, rs->pos + dlen, app.hide_cursor_syntax);
                            for (size_t i = 0; i < dlen && rs->pos < len; i++) {
                                track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                                            &rs->cursor_virtual_row, &rs->cursor_col);
                                if (cursor_in_delim) {
                                    set_fg(get_dim());
                                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                        rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                    else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                } else {
                                    rs->pos++;
                                }
                            }
                            if (cursor_in_delim) set_fg(get_fg());
                            for (int si = rs->style_depth - 1; si >= close_idx; si--) rs->active_style &= ~rs->style_stack[si].style;
                            rs->style_depth = close_idx;
                            reset_attrs();
                            set_bg(get_bg());
                            continue;
                        }

                        if (!(rs->active_style & delim) && rs->style_depth < MAX_STYLE_DEPTH) {
                            size_t close_pos = md_find_closing(&app.text, rs->pos, delim, dlen);
                            if (close_pos > 0) {
                                rs->style_stack[rs->style_depth].style = delim;
                                rs->style_stack[rs->style_depth].dlen = dlen;
                                rs->style_stack[rs->style_depth].close_pos = close_pos;
                                rs->style_depth++;
                                rs->active_style |= delim;

                                bool cursor_in_delim = cursor_in_range(app.cursor, rs->pos, rs->pos + dlen, app.hide_cursor_syntax);
                                for (size_t i = 0; i < dlen && rs->pos < len; i++) {
                                    track_cursor(rs->pos, rs->virtual_row, rs->col_width, ctx->L.margin,
                                                &rs->cursor_virtual_row, &rs->cursor_col);
                                    if (cursor_in_delim) {
                                        set_fg(get_dim());
                                        if (is_row_visible(&ctx->L, screen_row, ctx->max_row))
                                            rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                                        else { size_t next; rs->col_width += grapheme_width_next(&app.text, rs->pos, &next); rs->pos = next; }
                                    } else {
                                        rs->pos++;
                                    }
                                }
                                if (cursor_in_delim) set_fg(get_fg());
                                continue;
                            }
                        }
                    }

                    // Apply style and render character
                    if (rs->in_block_math) {
                        set_italic(true);
                        set_fg(get_accent());
                    } else if (rs->active_style) {
                        md_apply(rs->active_style);
                    } else if (rs->line_style) {
                        md_apply(rs->line_style);
                    } else {
                        md_apply(0);
                    }

                    if (!(rs->active_style & MD_MARK)) {
                        if (in_sel) set_bg(get_select());
                        else set_bg(get_bg());
                    }

                    if (is_row_visible(&ctx->L, screen_row, ctx->max_row)) {
                        rs->col_width += output_grapheme_advance(&app.text, &rs->pos);
                    } else {
                        size_t next;
                        rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                        rs->pos = next;
                    }
                }

                // End of segment - wrap to next line if needed
                if (rs->pos >= seg_end && rs->pos < line_end) {
                    rs->virtual_row += text_scale;
                    rs->col_width = 0;
                    rs->pos = skip_leading_space(&app.text, rs->pos, line_end);
                }
            }
            break;
        }
    }

    (void)cursor_in_block;
}
