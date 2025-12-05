// platform.h - Platform Abstraction Layer
//! Defines interfaces for platform-specific functionality
//! Implementations: platform_posix.c (macOS/Linux terminal)
//!                  platform_web.c (future: browser/canvas/xterm.js)

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


//! RGB color value
typedef struct {
    uint8_t r, g, b;
} PlatformColor;

//! Platform capability flags (bitmask)
typedef enum {
    PLATFORM_CAP_NONE            = 0,
    PLATFORM_CAP_TRUE_COLOR      = 1 << 0,   //!< 24-bit color support
    PLATFORM_CAP_SYNC_OUTPUT     = 1 << 1,   //!< Synchronized output
    PLATFORM_CAP_STYLED_UNDERLINE = 1 << 2,  //!< Curly/dotted underlines
    PLATFORM_CAP_TEXT_SIZING     = 1 << 3,   //!< Text scaling (Kitty protocol)
    PLATFORM_CAP_IMAGES          = 1 << 4,   //!< Image display support
    PLATFORM_CAP_MOUSE           = 1 << 5,   //!< Mouse input support
    PLATFORM_CAP_BRACKETED_PASTE = 1 << 6,   //!< Bracketed paste mode
    PLATFORM_CAP_FOCUS_EVENTS    = 1 << 7,   //!< Focus in/out events
    PLATFORM_CAP_CLIPBOARD       = 1 << 8,   //!< System clipboard access
} PlatformCapability;

//! Underline style types
typedef enum {
    PLATFORM_UNDERLINE_SINGLE,   //!< Standard underline
    PLATFORM_UNDERLINE_CURLY,    //!< Curly/wavy underline
    PLATFORM_UNDERLINE_DOTTED,   //!< Dotted underline
    PLATFORM_UNDERLINE_DASHED    //!< Dashed underline
} PlatformUnderlineStyle;

//! Key codes for special keys
typedef enum {
    PLATFORM_KEY_NONE = -1,

    // Arrow keys
    PLATFORM_KEY_UP = 1000,
    PLATFORM_KEY_DOWN,
    PLATFORM_KEY_RIGHT,
    PLATFORM_KEY_LEFT,

    // Navigation
    PLATFORM_KEY_HOME,
    PLATFORM_KEY_END,
    PLATFORM_KEY_PGUP,
    PLATFORM_KEY_PGDN,
    PLATFORM_KEY_DEL,

    // Shift+Arrow (selection)
    PLATFORM_KEY_SHIFT_UP,
    PLATFORM_KEY_SHIFT_DOWN,
    PLATFORM_KEY_SHIFT_LEFT,
    PLATFORM_KEY_SHIFT_RIGHT,

    // Ctrl+Arrow (word movement)
    PLATFORM_KEY_CTRL_LEFT,
    PLATFORM_KEY_CTRL_RIGHT,
    PLATFORM_KEY_CTRL_SHIFT_LEFT,
    PLATFORM_KEY_CTRL_SHIFT_RIGHT,

    // Alt+Arrow (word movement on macOS)
    PLATFORM_KEY_ALT_LEFT,
    PLATFORM_KEY_ALT_RIGHT,
    PLATFORM_KEY_ALT_SHIFT_LEFT,
    PLATFORM_KEY_ALT_SHIFT_RIGHT,

    // Alt+Up/Down (fast navigation)
    PLATFORM_KEY_ALT_UP,
    PLATFORM_KEY_ALT_DOWN,

    // Ctrl+Home/End (document start/end)
    PLATFORM_KEY_CTRL_HOME,
    PLATFORM_KEY_CTRL_END,

    // Mouse
    PLATFORM_KEY_MOUSE_SCROLL_UP,
    PLATFORM_KEY_MOUSE_SCROLL_DOWN,
    PLATFORM_KEY_MOUSE_CLICK,       //!< Left mouse button click

    // Special
    PLATFORM_KEY_BTAB  //!< Shift+Tab (backtab)
} PlatformKey;

//! Mouse event data
typedef struct {
    int x;           //!< Column (1-based)
    int y;           //!< Row (1-based)
    int button;      //!< Button number (0=left, 1=middle, 2=right)
    bool pressed;    //!< True if button pressed, false if released
    bool scroll_up;  //!< Scroll wheel up
    bool scroll_down;//!< Scroll wheel down
} PlatformMouseEvent;

//! Local time components
typedef struct {
    int year;   //!< Full year (e.g., 2024)
    int mon;    //!< Month (0-11)
    int mday;   //!< Day of month (1-31)
    int hour;   //!< Hour (0-23)
    int min;    //!< Minute (0-59)
    int sec;    //!< Second (0-59)
    int wday;   //!< Day of week (0-6, 0=Sunday)
} PlatformLocalTime;


//! Platform backend vtable - implement this for each platform
typedef struct PlatformBackend {
    const char *name;  //!< Backend name (e.g., "posix", "web")

    // --- Lifecycle ---

    //! Initialize the platform backend
    //! @return true on success
    bool (*init)(void);

    //! Shutdown the platform backend and restore state
    void (*shutdown)(void);

    //! Get detected capabilities
    //! @return bitmask of PlatformCapability flags
    uint32_t (*get_capabilities)(void);

    // --- Display ---

    //! Get display dimensions
    //! @param out_cols output: number of columns
    //! @param out_rows output: number of rows
    void (*get_size)(int *out_cols, int *out_rows);

    //! Set cursor position (1-based)
    //! @param col column number (1 = left)
    //! @param row row number (1 = top)
    void (*set_cursor)(int col, int row);

    //! Show or hide the cursor
    //! @param visible true to show, false to hide
    void (*set_cursor_visible)(bool visible);

    //! Set foreground (text) color
    //! @param color RGB color
    void (*set_fg)(PlatformColor color);

    //! Set background color
    //! @param color RGB color
    void (*set_bg)(PlatformColor color);

    //! Reset all text attributes
    void (*reset_attrs)(void);

    //! Set bold attribute
    //! @param enabled true to enable
    void (*set_bold)(bool enabled);

    //! Set italic attribute
    //! @param enabled true to enable
    void (*set_italic)(bool enabled);

    //! Set dim attribute
    //! @param enabled true to enable
    void (*set_dim)(bool enabled);

    //! Set strikethrough attribute
    //! @param enabled true to enable
    void (*set_strikethrough)(bool enabled);

    //! Set underline with style
    //! @param style underline style
    void (*set_underline)(PlatformUnderlineStyle style);

    //! Set underline color
    //! @param color RGB color for underline
    void (*set_underline_color)(PlatformColor color);

    //! Clear underline
    void (*clear_underline)(void);

    //! Clear entire screen
    void (*clear_screen)(void);

    //! Clear current line
    void (*clear_line)(void);

    //! Output a string
    //! @param str UTF-8 string to output
    //! @param len length in bytes
    void (*write_str)(const char *str, size_t len);

    //! Output a character
    //! @param c character to output
    void (*write_char)(char c);

    //! Output scaled text (if supported)
    //! @param str UTF-8 string to output
    //! @param len length in bytes
    //! @param scale scale factor (1-7)
    void (*write_scaled)(const char *str, size_t len, int scale);

    //! Output fractionally scaled text (if supported)
    //! Uses Kitty text sizing protocol with fractional scale n/d
    //! Effective font size = s * (n/d) where s is the cell scale
    //! @param str UTF-8 string to output
    //! @param len length in bytes
    //! @param scale cell scale factor (1-7)
    //! @param num numerator for fractional scale (0-15, 0 = no fractional)
    //! @param denom denominator for fractional scale (0-15, must be > num when non-zero)
    void (*write_scaled_frac)(const char *str, size_t len, int scale, int num, int denom);

    //! Flush output buffer
    void (*flush)(void);

    //! Begin synchronized output (reduces flicker)
    void (*sync_begin)(void);

    //! End synchronized output
    void (*sync_end)(void);

    //! Set terminal/window title
    //! @param title UTF-8 title string (NULL to reset to default)
    void (*set_title)(const char *title);

    // --- Input ---

    //! Read a key from input (non-blocking)
    //! @return key code or PLATFORM_KEY_NONE if no input
    int (*read_key)(void);

    //! Get last mouse event column
    //! @return column (1-based) or 0 if no mouse event
    int (*get_last_mouse_col)(void);

    //! Get last mouse event row
    //! @return row (1-based) or 0 if no mouse event
    int (*get_last_mouse_row)(void);

    //! Check if resize occurred since last check
    //! @return true if resize occurred
    bool (*check_resize)(void);

    //! Check if quit was requested
    //! @return true if quit requested
    bool (*check_quit)(void);

    //! Execute pending background jobs (async downloads, etc.)
    void (*execute_pending_jobs)(void);

    //! Check if input is available
    //! @param timeout_ms timeout in milliseconds (0 = non-blocking, -1 = block forever)
    //! @return true if input available
    bool (*input_available)(float timeout_ms);

    //! Register signal handlers
    //! @param on_resize callback for resize signals
    //! @param on_quit callback for quit signals (SIGINT, SIGTERM)
    void (*register_signals)(void (*on_resize)(int), void (*on_quit)(int));

    // --- Clipboard ---

    //! Copy text to system clipboard
    //! @param text text to copy
    //! @param len length in bytes
    void (*clipboard_copy)(const char *text, size_t len);

    //! Paste text from system clipboard
    //! @param out_len output: length of pasted text
    //! @return newly allocated string (caller must free), or NULL
    char *(*clipboard_paste)(size_t *out_len);

    // --- Filesystem ---

    //! Get user home directory path
    //! @return static string with home path, or NULL
    const char *(*get_home_dir)(void);

    //! Create directory (including parents)
    //! @param path directory path
    //! @return true on success
    bool (*mkdir_p)(const char *path);

    //! Check if file exists
    //! @param path file path
    //! @return true if exists
    bool (*file_exists)(const char *path);

    //! Read entire file contents
    //! @param path file path
    //! @param out_len output: file size
    //! @return newly allocated contents (caller must free), or NULL
    char *(*read_file)(const char *path, size_t *out_len);

    //! Write entire file contents
    //! @param path file path
    //! @param data data to write
    //! @param len length in bytes
    //! @return true on success
    bool (*write_file)(const char *path, const char *data, size_t len);

    //! List directory entries
    //! @param path directory path
    //! @param out_names output: array of entry names (caller must free each and array)
    //! @param out_count output: number of entries
    //! @return true on success
    bool (*list_dir)(const char *path, char ***out_names, int *out_count);

    //! Get file modification time
    //! @param path file path
    //! @return modification time in seconds since epoch, or 0 on error
    int64_t (*get_mtime)(const char *path);

    //! Delete a file
    //! @param path file path
    //! @return true on success
    bool (*delete_file)(const char *path);

    //! Open file in system file manager (e.g., Finder)
    //! @param path file path
    void (*reveal_in_finder)(const char *path);

    // --- Time ---

    //! Get current time in seconds since epoch
    //! @return seconds since Unix epoch
    int64_t (*time_now)(void);

    //! Sleep for specified milliseconds
    //! @param ms milliseconds to sleep
    void (*sleep_ms)(int ms);

    //! Get current local time components
    //! @param out local time struct to fill
    void (*get_local_time)(PlatformLocalTime *out);

    //! Get user's display name
    //! @return static string with display name, or NULL
    const char *(*get_username)(void);

    // --- Images ---

    //! Check if file is a supported image format
    //! @param path file path
    //! @return true if supported
    bool (*image_is_supported)(const char *path);

    //! Get image dimensions
    //! @param path file path
    //! @param out_width output: width in pixels
    //! @param out_height output: height in pixels
    //! @return true on success
    bool (*image_get_size)(const char *path, int *out_width, int *out_height);

    //! Display image at position
    //! @param path file path
    //! @param row row (1-based)
    //! @param col column (1-based)
    //! @param max_cols maximum width in columns
    //! @param max_rows maximum height in rows (0 = auto)
    //! @return number of rows occupied
    int (*image_display)(const char *path, int row, int col, int max_cols, int max_rows);

    //! Display image with vertical cropping
    //! @param path file path
    //! @param row row (1-based)
    //! @param col column (1-based)
    //! @param max_cols maximum width
    //! @param crop_top_rows rows to crop from top
    //! @param visible_rows visible rows
    //! @return number of rows occupied
    int (*image_display_cropped)(const char *path, int row, int col, int max_cols,
                                  int crop_top_rows, int visible_rows);

    //! Start new frame - clear image placements
    void (*image_frame_start)(void);

    //! End frame - finalize image display
    void (*image_frame_end)(void);

    //! Clear all images
    void (*image_clear_all)(void);

    //! Draw opaque mask over region (for popups over images)
    //! @param col column (1-based)
    //! @param row row (1-based)
    //! @param cols width
    //! @param rows height
    //! @param bg background color
    void (*image_mask_region)(int col, int row, int cols, int rows, PlatformColor bg);

    //! Resolve image path and cache if remote
    //! @param raw_path input path
    //! @param base_dir base directory for relative paths
    //! @param out output buffer
    //! @param out_size buffer size
    //! @return true on success
    bool (*image_resolve_path)(const char *raw_path, const char *base_dir,
                               char *out, size_t out_size);

    //! Calculate image rows given dimensions
    //! @param pixel_width image width
    //! @param pixel_height image height
    //! @param max_cols max display columns
    //! @param max_rows max display rows (0 = auto)
    //! @return calculated rows
    int (*image_calc_rows)(int pixel_width, int pixel_height, int max_cols, int max_rows);

    //! Invalidate image cache for path
    //! @param path file path
    void (*image_invalidate)(const char *path);

} PlatformBackend;


//! Initialize platform with specified backend
//! @param backend platform backend implementation
//! @return true on success
bool platform_init(const PlatformBackend *backend);

//! Shutdown platform
void platform_shutdown(void);

//! Current platform backend (set by platform_init)
//! Access via platform_get() or PLATFORM macro
extern const PlatformBackend *_platform_current;

//! Get current platform backend
//! @return current backend or NULL if not initialized
static inline const PlatformBackend *platform_get(void) {
    return _platform_current;
}

//! Check if platform has capability
//! @param cap capability to check
//! @return true if capability is available
static inline bool platform_has(PlatformCapability cap) {
    if (__builtin_expect(_platform_current == NULL, 0)) return false;
    if (__builtin_expect(_platform_current->get_capabilities == NULL, 0)) return false;
    return (_platform_current->get_capabilities() & (uint32_t)cap) != 0;
}

//! Convenience macro for platform access with NULL check
#define PLATFORM (_platform_current)


//! POSIX terminal backend (macOS, Linux)
extern const PlatformBackend platform_posix;

#ifdef __EMSCRIPTEN__
//! Web/Canvas backend (Emscripten)
extern const PlatformBackend platform_web;
#endif

// Future backends:
// extern const PlatformBackend platform_win32;  // Windows console

#endif // PLATFORM_H
