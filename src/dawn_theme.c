// dawn_theme.c

#include "dawn_theme.h"

// #region Color Palettes

//! Light theme - warm paper aesthetic
static const Color LIGHT_BG        = { 252, 250, 245 };  //!< Cream paper background
static const Color LIGHT_FG        = { 45, 45, 45 };     //!< Dark ink text
static const Color LIGHT_DIM       = { 160, 155, 145 };  //!< Muted annotations
static const Color LIGHT_ACCENT    = { 120, 100, 80 };   //!< Sepia accent
static const Color LIGHT_SELECT    = { 255, 245, 200 };  //!< Warm highlight
static const Color LIGHT_AI_BG     = { 245, 243, 238 };  //!< Subtle AI panel
static const Color LIGHT_BORDER    = { 220, 215, 205 };  //!< Soft borders
static const Color LIGHT_CODE_BG   = { 240, 238, 233 };  //!< Code block background
static const Color LIGHT_MODAL_BG  = { 255, 253, 250 };  //!< Modal popup background

//! Dark theme - deep focus aesthetic
static const Color DARK_BG         = { 22, 22, 26 };     //!< Deep charcoal background
static const Color DARK_FG         = { 210, 205, 195 };  //!< Warm white text
static const Color DARK_DIM        = { 90, 85, 80 };     //!< Muted annotations
static const Color DARK_ACCENT     = { 200, 175, 130 };  //!< Golden accent
static const Color DARK_SELECT     = { 60, 55, 45 };     //!< Subtle highlight
static const Color DARK_AI_BG      = { 28, 28, 32 };     //!< Slightly lighter panel
static const Color DARK_BORDER     = { 50, 48, 45 };     //!< Soft borders
static const Color DARK_CODE_BG    = { 30, 30, 34 };     //!< Code block background
static const Color DARK_MODAL_BG   = { 35, 35, 40 };     //!< Modal popup background

// #endregion

// #region Output Primitives

void set_fg(Color c) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_fg) {
        PlatformColor pc = {c.r, c.g, c.b};
        p->set_fg(pc);
    }
}

void set_bg(Color c) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_bg) {
        PlatformColor pc = {c.r, c.g, c.b};
        p->set_bg(pc);
    }
}

void move_to(int r, int c) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_cursor) {
        p->set_cursor(c, r);  // Note: platform uses (col, row) order
    }
}

void out_str(const char *str) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_str) {
        p->write_str(str, strlen(str));
    }
}

void out_str_n(const char *str, size_t len) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_str) {
        p->write_str(str, len);
    }
}

void out_char(char c) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_char) {
        p->write_char(c);
    }
}

void out_spaces(int n) {
    const PlatformBackend *p = platform_get();
    if (p && p->write_char) {
        for (int i = 0; i < n; i++) {
            p->write_char(' ');
        }
    }
}

void out_int(int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    out_str(buf);
}

void out_flush(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->flush) {
        p->flush();
    }
}

void clear_screen(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->clear_screen) {
        p->clear_screen();
    }
}

void clear_line(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->clear_line) {
        p->clear_line();
    }
}

void cursor_visible(bool visible) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_cursor_visible) {
        p->set_cursor_visible(visible);
    }
}

void cursor_home(void) {
    move_to(1, 1);
}

void sync_begin(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->sync_begin) {
        p->sync_begin();
    }
}

void sync_end(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->sync_end) {
        p->sync_end();
    }
}

// #endregion

// #region Theme Colors

Color get_bg(void) { return app.theme == THEME_DARK ? DARK_BG : LIGHT_BG; }
Color get_fg(void) { return app.theme == THEME_DARK ? DARK_FG : LIGHT_FG; }
Color get_dim(void) { return app.theme == THEME_DARK ? DARK_DIM : LIGHT_DIM; }
Color get_accent(void) { return app.theme == THEME_DARK ? DARK_ACCENT : LIGHT_ACCENT; }
Color get_select(void) { return app.theme == THEME_DARK ? DARK_SELECT : LIGHT_SELECT; }
Color get_ai_bg(void) { return app.theme == THEME_DARK ? DARK_AI_BG : LIGHT_AI_BG; }
Color get_border(void) { return app.theme == THEME_DARK ? DARK_BORDER : LIGHT_BORDER; }
Color get_code_bg(void) { return app.theme == THEME_DARK ? DARK_CODE_BG : LIGHT_CODE_BG; }
Color get_modal_bg(void) { return app.theme == THEME_DARK ? DARK_MODAL_BG : LIGHT_MODAL_BG; }

// #endregion

// #region Color Utilities

Color color_lerp(Color a, Color b, float t) {
    return (Color){
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t)
    };
}

// #endregion

// #region Text Attributes

void set_bold(bool on) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_bold) p->set_bold(on);
}

void set_italic(bool on) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_italic) p->set_italic(on);
}

void set_dim(bool on) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_dim) p->set_dim(on);
}

void set_strikethrough(bool on) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_strikethrough) p->set_strikethrough(on);
}

void reset_attrs(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->reset_attrs) p->reset_attrs();
}

// #endregion

// #region Styled Text

void set_underline(UnderlineStyle style) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_underline) {
        p->set_underline(style);
    }
}

void set_underline_color(Color c) {
    const PlatformBackend *p = platform_get();
    if (p && p->set_underline_color) {
        PlatformColor pc = {c.r, c.g, c.b};
        p->set_underline_color(pc);
    }
}

void clear_underline(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->clear_underline) {
        p->clear_underline();
    }
}

// #endregion

// #region Text Sizing

void print_scaled_char(char c, int scale) {
    const PlatformBackend *p = platform_get();
    if (scale <= 1 || !platform_has(PLATFORM_CAP_TEXT_SIZING)) {
        if (p && p->write_char) {
            p->write_char(c);
        }
        return;
    }
    if (p && p->write_scaled) {
        char str[2] = {c, '\0'};
        p->write_scaled(str, 1, scale);
    }
}

void print_scaled_str(const char *str, size_t len, int scale) {
    const PlatformBackend *p = platform_get();
    if (scale <= 1 || !platform_has(PLATFORM_CAP_TEXT_SIZING)) {
        if (p && p->write_str) {
            p->write_str(str, len);
        }
        return;
    }
    if (p && p->write_scaled) {
        p->write_scaled(str, len, scale);
    }
}

void print_scaled_frac_char(char c, int scale, int num, int denom) {
    const PlatformBackend *p = platform_get();
    // No scaling needed if scale is 1 with no fractional part, or no text sizing support
    if ((scale <= 1 && (num == 0 || denom == 0)) || !platform_has(PLATFORM_CAP_TEXT_SIZING)) {
        if (p && p->write_char) {
            p->write_char(c);
        }
        return;
    }
    if (p && p->write_scaled_frac) {
        char str[2] = {c, '\0'};
        p->write_scaled_frac(str, 1, scale, num, denom);
    } else if (p && p->write_scaled && scale > 1) {
        // Fallback to integer scaling if fractional not supported
        char str[2] = {c, '\0'};
        p->write_scaled(str, 1, scale);
    } else if (p && p->write_char) {
        p->write_char(c);
    }
}

void print_scaled_frac_str(const char *str, size_t len, int scale, int num, int denom) {
    const PlatformBackend *p = platform_get();
    // No scaling needed if scale is 1 with no fractional part, or no text sizing support
    if ((scale <= 1 && (num == 0 || denom == 0)) || !platform_has(PLATFORM_CAP_TEXT_SIZING)) {
        if (p && p->write_str) {
            p->write_str(str, len);
        }
        return;
    }
    if (p && p->write_scaled_frac) {
        p->write_scaled_frac(str, len, scale, num, denom);
    } else if (p && p->write_scaled && scale > 1) {
        // Fallback to integer scaling if fractional not supported
        p->write_scaled(str, len, scale);
    } else if (p && p->write_str) {
        p->write_str(str, len);
    }
}

// #endregion
