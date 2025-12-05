// dawn_theme.h

#ifndef DAWN_THEME_H
#define DAWN_THEME_H

#include "dawn_types.h"

// #region Output Primitives

//! Set foreground (text) color
//! @param c RGB color to set
void set_fg(Color c);

//! Set background color
//! @param c RGB color to set
void set_bg(Color c);

//! Move cursor to row and column (1-indexed)
//! @param r row number (1 = top)
//! @param c column number (1 = left)
void move_to(int r, int c);

//! Write a string to output
void out_str(const char *str);

//! Write a string with explicit length
void out_str_n(const char *str, size_t len);

//! Write a single character
void out_char(char c);

//! Write N spaces
void out_spaces(int n);

//! Write an integer
void out_int(int value);

//! Flush output buffer
void out_flush(void);

//! Clear the entire screen
void clear_screen(void);

//! Clear current line
void clear_line(void);

//! Show or hide the cursor
void cursor_visible(bool visible);

//! Move cursor to home position (1,1)
void cursor_home(void);

//! Begin synchronized output
void sync_begin(void);

//! End synchronized output
void sync_end(void);

// #endregion

// #region Theme Colors

//! Get current theme's background color
Color get_bg(void);

//! Get current theme's foreground (text) color
Color get_fg(void);

//! Get current theme's dimmed/muted text color
Color get_dim(void);

//! Get current theme's accent color (highlights, links)
Color get_accent(void);

//! Get current theme's selection highlight color
Color get_select(void);

//! Get current theme's AI panel background color
Color get_ai_bg(void);

//! Get current theme's border/separator color
Color get_border(void);

//! Get current theme's code block background color
Color get_code_bg(void);

//! Get current theme's modal popup background color
Color get_modal_bg(void);

// #endregion

// #region Color Utilities

//! Linear interpolation between two colors
//! @param a start color
//! @param b end color
//! @param t interpolation factor (0.0 = a, 1.0 = b)
//! @return interpolated color
Color color_lerp(Color a, Color b, float t);

// #endregion

// #region Text Attributes

//! Set bold text attribute
void set_bold(bool on);

//! Set italic text attribute
void set_italic(bool on);

//! Set dim/faint text attribute
void set_dim(bool on);

//! Set strikethrough text attribute
void set_strikethrough(bool on);

//! Reset all text attributes
void reset_attrs(void);

// #endregion

// #region Styled Text

//! Underline style types (for compatibility)
typedef PlatformUnderlineStyle UnderlineStyle;
#define UNDERLINE_STYLE_SINGLE PLATFORM_UNDERLINE_SINGLE
#define UNDERLINE_STYLE_CURLY  PLATFORM_UNDERLINE_CURLY
#define UNDERLINE_STYLE_DOTTED PLATFORM_UNDERLINE_DOTTED
#define UNDERLINE_STYLE_DASHED PLATFORM_UNDERLINE_DASHED

//! Set underline with style (uses styled underlines if supported)
//! @param style desired underline style
void set_underline(UnderlineStyle style);

//! Set underline color
//! @param c RGB color for underline
void set_underline_color(Color c);

//! Clear underline styling
void clear_underline(void);

// #endregion

// #region Text Sizing

//! Print a single character at scaled size
//! @param c the character to print
//! @param scale integer scale 1-7 (1 = normal, 2 = double, etc.)
//! Falls back to normal print if platform doesn't support text sizing
void print_scaled_char(char c, int scale);

//! Print a string at scaled size
//! @param str the string to print
//! @param len length of string
//! @param scale integer scale 1-7 (1 = normal, 2 = double, etc.)
//! Falls back to normal print if platform doesn't support text sizing
void print_scaled_str(const char *str, size_t len, int scale);

//! Print a single character at fractionally scaled size
//! Uses Kitty text sizing protocol: effective size = scale * (num/denom)
//! @param c the character to print
//! @param scale cell scale 1-7 (1 = normal)
//! @param num fractional numerator (0-15, 0 = no fraction)
//! @param denom fractional denominator (0-15, must be > num when non-zero)
//! Falls back to integer scaling or normal print if not supported
void print_scaled_frac_char(char c, int scale, int num, int denom);

//! Print a string at fractionally scaled size
//! Uses Kitty text sizing protocol: effective size = scale * (num/denom)
//! @param str the string to print
//! @param len length of string
//! @param scale cell scale 1-7 (1 = normal)
//! @param num fractional numerator (0-15, 0 = no fraction)
//! @param denom fractional denominator (0-15, must be > num when non-zero)
//! Falls back to integer scaling or normal print if not supported
void print_scaled_frac_str(const char *str, size_t len, int scale, int num, int denom);

// #endregion

#endif // DAWN_THEME_H
