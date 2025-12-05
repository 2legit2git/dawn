// dawn_render.c

#include "dawn_render.h"
#include "dawn_gap.h"
#include "dawn_image.h"
#include "dawn_theme.h"
#include "dawn_timer.h"
#include "dawn_utils.h"
#include "dawn_toc.h"
#include "dawn_search.h"
#include <stdio.h>
#include <string.h>

// #region Platform Output Helpers

static void platform_write_str(const char *str) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_str) {
        p->write_str(str, strlen(str));
    }
}

static void platform_write_char(char c) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_char) {
        p->write_char(c);
    }
}

static void platform_clear_screen(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->clear_screen) {
        p->clear_screen();
    }
}

static void platform_set_cursor_visible(bool visible) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_cursor_visible) {
        p->set_cursor_visible(visible);
    }
}

static void platform_set_bold(bool enabled) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_bold) {
        p->set_bold(enabled);
    }
}

static void platform_reset_attrs(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->reset_attrs) {
        p->reset_attrs();
    }
}

// #endregion

// #region Utility Functions

void render_clear(void) {
    set_bg(get_bg());
    platform_clear_screen();
    for (int r = 0; r < app.rows; r++) {
        move_to(r + 1, 1);
        for (int c = 0; c < app.cols; c++) platform_write_char(' ');
    }
}

void render_center_text(int row, const char *text, Color fg) {
    int len = (int)strlen(text);
    int col = (app.cols - len) / 2;
    if (col < 1) col = 1;
    move_to(row, col);
    set_fg(fg);
    platform_write_str(text);
}

void render_popup_box(int width, int height, int *out_top, int *out_left) {
    int top = (app.rows - height) / 2;
    int left = (app.cols - width) / 2;
    if (top < 1) top = 1;
    if (left < 1) left = 1;

    Color bg = get_modal_bg();
    image_mask_region(left, top, width, height, bg);
    Color border = get_border();

    // Top border
    move_to(top, left);
    set_bg(bg);
    set_fg(border);
    platform_write_str("╭");
    for (int i = 0; i < width - 2; i++) platform_write_str("─");
    platform_write_str("╮");

    // Middle rows
    for (int r = 1; r < height - 1; r++) {
        move_to(top + r, left);
        set_fg(border);
        platform_write_str("│");
        set_fg(get_fg());
        for (int i = 0; i < width - 2; i++) platform_write_char(' ');
        set_fg(border);
        platform_write_str("│");
    }

    // Bottom border
    move_to(top + height - 1, left);
    set_fg(border);
    platform_write_str("╰");
    for (int i = 0; i < width - 2; i++) platform_write_str("─");
    platform_write_str("╯");

    if (out_top) *out_top = top;
    if (out_left) *out_left = left;
}

// #endregion

// #region Screen Renderers

static void render_text_at(int row, int col, const char *text, Color fg) {
    move_to(row, col);
    set_fg(fg);
    platform_write_str(text);
}

void render_welcome(void) {
    render_clear();

    // Use most of the available space
    int margin_h = app.cols > 100 ? 8 : (app.cols > 60 ? 4 : 2);
    int margin_v = app.rows > 30 ? 3 : 2;
    int content_left = margin_h + 1;
    int content_right = app.cols - margin_h;
    int content_width = content_right - content_left;

    // Vertical layout
    int top_row = margin_v + 1;
    int bottom_row = app.rows - margin_v;
    int center_row = (top_row + bottom_row) / 2;

    // Clean block letter logo
    static const char *logo[] = {
        "█▀▄ ▄▀█ █ █ █ █▄ █",
        "█▄▀ █▀█ ▀▄▀▄▀ █ ▀█",
    };
    int logo_height = 2;
    int logo_width = 19;

    // Center logo vertically - position it above center
    int logo_start = center_row - logo_height - 2;
    if (logo_start < top_row) logo_start = top_row;

    set_fg(get_fg());
    for (int i = 0; i < logo_height; i++) {
        int col = (app.cols - logo_width) / 2;
        if (col < 1) col = 1;
        move_to(logo_start + i, col);
        platform_write_str(logo[i]);
    }

    // Tagline below logo
    render_center_text(logo_start + logo_height + 1, "draft anything, write now", get_dim());

    // Actions grid - positioned below center
    int actions_row = center_row + 2;
    int col1 = content_left + content_width / 4 - 8;
    int col2 = content_left + content_width / 2 + content_width / 4 - 8;
    if (col1 < content_left + 2) col1 = content_left + 2;
    if (col2 < col1 + 20) col2 = col1 + 20;

    int row = actions_row;
    render_text_at(row, col1, "enter", get_accent());
    render_text_at(row, col1 + 6, " write", get_dim());
    render_text_at(row, col2, "h", get_accent());
    render_text_at(row, col2 + 2, " history", get_dim());

    row += 2;
    render_text_at(row, col1, "t", get_accent());
    render_text_at(row, col1 + 6, " timer", get_dim());
    render_text_at(row, col2, "d", get_accent());
    render_text_at(row, col2 + 2, " theme", get_dim());

    row += 2;
    render_text_at(row, col1, "q", get_accent());
    render_text_at(row, col1 + 6, " quit", get_dim());
    render_text_at(row, col2, "?", get_accent());
    render_text_at(row, col2 + 2, " help", get_dim());

    #if HAS_LIBAI
    if (app.ai_ready) {
        row += 2;
        render_center_text(row, "✦ ai ready", get_accent());
    }
    #endif

    // Bottom status bar - like editor status bar
    move_to(bottom_row, content_left);

    // Left: timer setting
    set_fg(get_dim());
    if (app.timer_mins == 0) {
        platform_write_str("no timer");
    } else {
        char timer_str[16];
        snprintf(timer_str, sizeof(timer_str), "%d min", app.timer_mins);
        platform_write_str(timer_str);
    }

    // Right: theme
    const char *theme_str = app.theme == THEME_DARK ? "dark" : "light";
    int theme_col = content_right - (int)strlen(theme_str);
    move_to(bottom_row, theme_col);
    set_fg(get_dim());
    platform_write_str(theme_str);
}

void render_timer_select(void) {
    render_clear();
    int cy = app.rows / 2;

    render_center_text(cy - 5, "select timer", get_fg());

    for (size_t i = 0; i < NUM_PRESETS; i++) {
        char buf[32];
        if (TIMER_PRESETS[i] == 0) {
            snprintf(buf, sizeof(buf), "%s no timer %s",
                     (int)i == app.preset_idx ? ">" : " ",
                     (int)i == app.preset_idx ? "<" : " ");
        } else {
            snprintf(buf, sizeof(buf), "%s %d min %s",
                     (int)i == app.preset_idx ? ">" : " ",
                     TIMER_PRESETS[i],
                     (int)i == app.preset_idx ? "<" : " ");
        }
        render_center_text(cy - 2 + (int)i, buf,
                    (int)i == app.preset_idx ? get_accent() : get_dim());
    }

    render_center_text(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
}

void render_style_select(void) {
    render_clear();
    int cy = app.rows / 2;

    render_center_text(cy - 4, "select style", get_fg());

    const char *names[] = {"minimal", "typewriter", "elegant"};
    const char *descs[] = {"clean focus", "monospace feel", "italic grace"};

    for (int i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s %s",
                 i == (int)app.style ? ">" : " ",
                 names[i],
                 i == (int)app.style ? "<" : " ");
        render_center_text(cy - 1 + i * 2, buf, i == (int)app.style ? get_accent() : get_dim());
        render_center_text(cy + i * 2, descs[i], get_dim());
    }

    render_center_text(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
}

void render_help(void) {
    int width = 44;
    int height = 26;
    int top, left;
    render_popup_box(width, height, &top, &left);

    int col1 = left + 4;
    int col2 = left + 20;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 9);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("KEYBOARD SHORTCUTS");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    int cy = top + 4;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("NAVIGATION");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1); platform_write_str("arrows");
    move_to(cy++, col2); platform_write_str("move cursor");
    move_to(cy, col1); platform_write_str("opt+arrows");
    move_to(cy++, col2); platform_write_str("word jump");
    move_to(cy, col1); platform_write_str("pgup/pgdn");
    move_to(cy++, col2); platform_write_str("scroll page");
    move_to(cy, col1); platform_write_str("^L");
    move_to(cy++, col2); platform_write_str("table of contents");
    move_to(cy, col1); platform_write_str("^S");
    move_to(cy++, col2); platform_write_str("search document");

    cy++;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("EDITING");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1); platform_write_str("^C ^X ^V");
    move_to(cy++, col2); platform_write_str("copy/cut/paste");
    move_to(cy, col1); platform_write_str("^Z ^Y");
    move_to(cy++, col2); platform_write_str("undo/redo");
    move_to(cy, col1); platform_write_str("^W ^D");
    move_to(cy++, col2); platform_write_str("delete word/elem");
    move_to(cy, col1); platform_write_str("tab shift+tab");
    move_to(cy++, col2); platform_write_str("indent list");

    cy++;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("FEATURES");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1); platform_write_str("^F");
    move_to(cy++, col2); platform_write_str("focus mode");
    move_to(cy, col1); platform_write_str("^R");
    move_to(cy++, col2); platform_write_str("plain text mode");
    move_to(cy, col1); platform_write_str("^G ^E");
    move_to(cy++, col2); platform_write_str("edit title/image");
    move_to(cy, col1); platform_write_str("^P ^T");
    move_to(cy++, col2); platform_write_str("pause/timer");
    #if HAS_LIBAI
    move_to(cy, col1); platform_write_str("^/");
    move_to(cy++, col2); platform_write_str("AI chat");
    #endif

    // Footer
    move_to(top + height - 2, left + (width - 22) / 2);
    set_fg(get_dim());
    platform_write_str("press any key to close");
}

void render_history(void) {
    render_clear();

    if (app.hist_count == 0) {
        render_center_text(app.rows / 2, "no history yet", get_dim());
        render_center_text(app.rows / 2 + 2, "[esc] back", get_dim());
        return;
    }

    move_to(2, 4);
    set_fg(get_fg());
    platform_write_str("history");

    int visible = app.rows - 6;
    int start = 0;
    if (app.hist_sel >= visible) start = app.hist_sel - visible + 1;

    for (int i = 0; i < visible && start + i < app.hist_count; i++) {
        int idx = start + i;
        HistoryEntry *entry = &app.history[idx];

        move_to(4 + i, 4);
        if (idx == app.hist_sel) {
            set_fg(get_accent());
            platform_write_str("> ");
        } else {
            set_fg(get_dim());
            platform_write_str("  ");
        }

        // Display title (or "Untitled") followed by date
        const char *title = entry->title ? entry->title : "Untitled";
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "%-30.30s  ", title);
        platform_write_str(title_buf);
        set_fg(get_dim());
        platform_write_str(entry->date_str);
    }

    move_to(app.rows - 1, 4);
    set_fg(get_dim());
    platform_write_str("[j/k] select   [o] open   [t] title   [d] delete   [e] finder   [esc] back");
}

void render_finished(void) {
    render_clear();
    int cy = app.rows / 2;

    render_center_text(cy - 3, "done.", get_fg());
    render_center_text(cy - 1, "your writing is saved.", get_dim());

    int words = count_words(&app.text);

    char stats[64];
    if (app.timer_start > 0) {
        const PlatformBackend *p = platform_get();
        int64_t now = p && p->time_now ? p->time_now() : 0;
        int64_t elapsed_secs;
        if (app.timer_paused) {
            elapsed_secs = app.timer_mins * 60 - app.timer_paused_at;
        } else {
            elapsed_secs = now - app.timer_start;
        }
        int elapsed_mins = (int)(elapsed_secs / 60);
        if (elapsed_mins < 1) elapsed_mins = 1;
        snprintf(stats, sizeof(stats), "%d words in %d min", words, elapsed_mins);
    } else {
        snprintf(stats, sizeof(stats), "%d words", words);
    }
    render_center_text(cy + 1, stats, get_accent());

    render_center_text(cy + 4, "[c] continue   [enter] new   [esc] menu", get_dim());
    render_center_text(cy + 5, "[o] finder   [q] quit", get_dim());
    #if HAS_LIBAI
    if (app.ai_ready) {
        render_center_text(cy + 7, "[/] reflect with ai", get_dim());
    }
    #endif
}

void render_title_edit(void) {
    int box_width = 50;
    int box_height = 7;
    int top, left;

    render_popup_box(box_width, box_height, &top, &left);

    int content_left = left + 2;
    int content_top = top + 1;

    set_bg(get_modal_bg());

    // Title
    move_to(content_top, content_left);
    set_fg(get_dim());
    platform_write_str("Set Title");

    // Input field
    int input_row = content_top + 2;
    move_to(input_row, content_left);
    set_fg(get_accent());
    platform_write_str("> ");
    set_fg(get_fg());
    for (size_t i = 0; i < app.title_edit_len; i++) {
        platform_write_char(app.title_edit_buf[i]);
    }

    // Help text
    move_to(content_top + 4, content_left);
    set_fg(get_dim());
    platform_write_str("enter:save  esc:cancel");

    // Position cursor
    move_to(input_row, content_left + 2 + (int)app.title_edit_cursor);
    platform_set_cursor_visible(true);
}

void render_image_edit(void) {
    int box_width = 50;
    int box_height = 9;
    int top, left;

    render_popup_box(box_width, box_height, &top, &left);

    int content_left = left + 2;
    int content_top = top + 1;

    set_bg(get_modal_bg());

    // Title
    move_to(content_top, content_left);
    set_fg(get_dim());
    platform_write_str("Edit Image Dimensions");

    // Width field
    int field_row = content_top + 2;
    move_to(field_row, content_left);
    set_fg(app.img_edit_field == 0 ? get_accent() : get_dim());
    platform_write_str("Width:  ");
    set_fg(get_fg());

    for (size_t i = 0; i < app.img_edit_width_len; i++) {
        platform_write_char(app.img_edit_width_buf[i]);
    }
    set_fg(get_dim());
    if (app.img_edit_width_pct) platform_write_str("%");
    else platform_write_str("px");

    // Height field
    move_to(field_row + 1, content_left);
    set_fg(app.img_edit_field == 1 ? get_accent() : get_dim());
    platform_write_str("Height: ");
    set_fg(get_fg());

    for (size_t i = 0; i < app.img_edit_height_len; i++) {
        platform_write_char(app.img_edit_height_buf[i]);
    }
    set_fg(get_dim());
    if (app.img_edit_height_pct) platform_write_str("%");
    else platform_write_str("px");

    // Help text
    move_to(content_top + 5, content_left);
    set_fg(get_dim());
    platform_write_str("tab:field  p:%/px  enter:save  esc:cancel");

    // Position cursor
    if (app.img_edit_field == 0) {
        move_to(field_row, content_left + 8 + (int)app.img_edit_width_len);
    } else {
        move_to(field_row + 1, content_left + 8 + (int)app.img_edit_height_len);
    }
    platform_set_cursor_visible(true);
}

void render_toc(void) {
    TocState *toc = (TocState *)app.toc_state;
    if (!toc) return;

    // Calculate dimensions
    int width = app.cols > 80 ? 70 : app.cols - 6;
    int max_height = app.rows - 6;
    int list_height = max_height - 7;  // Space for header, filter, footer
    if (list_height < 3) list_height = 3;
    int height = list_height + 7;

    int top, left;
    render_popup_box(width, height, &top, &left);

    int content_left = left + 3;
    int content_right = left + width - 3;
    int content_width = content_right - content_left;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 8);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("TABLE OF CONTENTS");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    // Filter input
    int filter_row = top + 4;
    move_to(filter_row, content_left);
    set_fg(get_dim());
    platform_write_str("filter: ");
    set_fg(get_accent());
    for (int i = 0; i < toc->filter_len && i < content_width - 10; i++) {
        platform_write_char(toc->filter[i]);
    }
    // Cursor indicator
    set_fg(get_fg());
    platform_write_char('_');

    // Results count
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d/%d", toc->filtered_count, toc->count);
    move_to(filter_row, content_right - (int)strlen(count_str));
    set_fg(get_dim());
    platform_write_str(count_str);

    // Separator
    move_to(top + 5, content_left);
    set_fg(get_border());
    for (int i = 0; i < content_width; i++) platform_write_str("─");

    // TOC entries
    int list_start = top + 6;
    int visible = list_height;

    // Adjust scroll to keep selection visible
    if (toc->selected < toc->scroll) toc->scroll = toc->selected;
    if (toc->selected >= toc->scroll + visible) toc->scroll = toc->selected - visible + 1;

    for (int i = 0; i < visible; i++) {
        int idx = toc->scroll + i;
        if (idx >= toc->filtered_count) break;

        int entry_idx = toc->filtered[idx];
        TocEntry *entry = &toc->entries[entry_idx];

        move_to(list_start + i, content_left);

        // Selection indicator
        if (idx == toc->selected) {
            set_fg(get_accent());
            platform_write_str("▸ ");
        } else {
            platform_write_str("  ");
        }

        // Indentation based on hierarchy depth
        int indent = entry->depth * 2;
        for (int j = 0; j < indent && j < 12; j++) platform_write_char(' ');

        // Header text
        set_fg(idx == toc->selected ? get_fg() : get_dim());
        if (idx == toc->selected) platform_set_bold(true);

        // Truncate if needed
        int max_text = content_width - 4 - indent;
        int text_len = entry->text_len;
        if (text_len > max_text) text_len = max_text;

        for (int j = 0; j < text_len; j++) {
            platform_write_char(entry->text[j]);
        }
        if (entry->text_len > max_text) {
            set_fg(get_dim());
            platform_write_str("...");
        }

        platform_reset_attrs();
        set_bg(get_modal_bg());
    }

    // Scroll indicators
    if (toc->scroll > 0) {
        move_to(list_start, content_right);
        set_fg(get_dim());
        platform_write_str("↑");
    }
    if (toc->scroll + visible < toc->filtered_count) {
        move_to(list_start + visible - 1, content_right);
        set_fg(get_dim());
        platform_write_str("↓");
    }

    // Footer
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    platform_write_str("↑↓:nav  enter:jump  esc:close");

    // Position cursor at filter
    move_to(filter_row, content_left + 8 + toc->filter_len);
    platform_set_cursor_visible(true);
}

void render_search(void) {
    SearchState *search = (SearchState *)app.search_state;
    if (!search) return;

    // Calculate dimensions
    int width = app.cols > 90 ? 80 : app.cols - 6;
    int max_height = app.rows - 6;
    int list_height = max_height - 8;
    if (list_height < 3) list_height = 3;
    int height = list_height + 8;

    int top, left;
    render_popup_box(width, height, &top, &left);

    int content_left = left + 3;
    int content_right = left + width - 3;
    int content_width = content_right - content_left;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 3);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("SEARCH");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    // Search input
    int search_row = top + 4;
    move_to(search_row, content_left);
    set_fg(get_dim());
    platform_write_str("find: ");
    set_fg(get_accent());
    for (int i = 0; i < search->query_len && i < content_width - 8; i++) {
        platform_write_char(search->query[i]);
    }
    set_fg(get_fg());
    platform_write_char('_');

    // Results count
    char count_str[32];
    if (search->count >= SEARCH_MAX_RESULTS) {
        snprintf(count_str, sizeof(count_str), "%d+ matches", search->count);
    } else {
        snprintf(count_str, sizeof(count_str), "%d match%s", search->count, search->count == 1 ? "" : "es");
    }
    move_to(search_row, content_right - (int)strlen(count_str));
    set_fg(get_dim());
    platform_write_str(count_str);

    // Separator
    move_to(top + 5, content_left);
    set_fg(get_border());
    for (int i = 0; i < content_width; i++) platform_write_str("─");

    // Search results with context
    int list_start = top + 6;
    int visible = list_height;

    // Adjust scroll
    if (search->selected < search->scroll) search->scroll = search->selected;
    if (search->selected >= search->scroll + visible) search->scroll = search->selected - visible + 1;

    for (int i = 0; i < visible; i++) {
        int idx = search->scroll + i;
        if (idx >= search->count) break;

        SearchResult *r = &search->results[idx];

        move_to(list_start + i, content_left);

        // Selection indicator
        if (idx == search->selected) {
            set_fg(get_accent());
            platform_write_str("▸ ");
        } else {
            platform_write_str("  ");
        }

        // Line number
        char line_str[16];
        snprintf(line_str, sizeof(line_str), "%4d: ", r->line_num);
        set_fg(get_dim());
        platform_write_str(line_str);

        // Context with highlighted match
        int max_ctx = content_width - 10;

        for (int j = 0; j < r->context_len && j < max_ctx; j++) {
            // Highlight the match
            if (j >= r->match_start && j < r->match_start + r->match_len) {
                set_fg(get_accent());
                if (idx == search->selected) platform_set_bold(true);
            } else {
                set_fg(idx == search->selected ? get_fg() : get_dim());
            }
            platform_write_char(r->context[j]);
            platform_reset_attrs();
            set_bg(get_modal_bg());
        }

        if (r->context_len > max_ctx) {
            set_fg(get_dim());
            platform_write_str("...");
        }
    }

    // Scroll indicators
    if (search->scroll > 0) {
        move_to(list_start, content_right);
        set_fg(get_dim());
        platform_write_str("↑");
    }
    if (search->scroll + visible < search->count) {
        move_to(list_start + visible - 1, content_right);
        set_fg(get_dim());
        platform_write_str("↓");
    }

    // Footer
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    platform_write_str("↑↓:nav  enter:jump  ^n/^p:next/prev  esc:close");

    // Position cursor at search
    move_to(search_row, content_left + 6 + search->query_len);
    platform_set_cursor_visible(true);
}

// #endregion
