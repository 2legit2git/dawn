// dawn_types.h

#ifndef DAWN_TYPES_H
#define DAWN_TYPES_H

// #region Standard Library Includes (Platform-Independent)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// #endregion

// #region Platform Abstraction

#include "platform.h"

// Re-export platform Color type for compatibility
typedef PlatformColor Color;

// #endregion

// #region External Dependencies

#include "utf8proc/utf8proc.h"

//! Apple Intelligence integration (macOS 26.0+)
#ifdef USE_LIBAI
    #include "ai.h"
    #include "search.h"
    #define HAS_LIBAI 1
#else
    #define HAS_LIBAI 0
#endif

// #endregion

// #region Application Configuration

#define APP_NAME "dawn"
#define APP_TAGLINE "Draft Anything, Write Now"
#define VERSION "1.0.0"

//! Maximum document size (1MB)
#define MAX_TEXT_SIZE (1024 * 1024)

//! Directory name for storing sessions in user's home
#define HISTORY_DIR_NAME ".dawn"

//! Default writing timer duration
#define DEFAULT_TIMER_MINUTES 15

//! Gap buffer initial gap size
#define GAP_BUFFER_GAP_SIZE 1024

//! AI chat panel width in columns
#define AI_PANEL_WIDTH 45

//! Maximum AI response size
#define MAX_AI_RESPONSE (64 * 1024)

//! Maximum AI input size
#define MAX_AI_INPUT 4096

//! Maximum lines in AI input area
#define AI_INPUT_MAX_LINES 6

//! Timer preset options (minutes)
static const int TIMER_PRESETS[] = { 0, 5, 10, 15, 20, 25, 30 };
#define NUM_PRESETS (sizeof(TIMER_PRESETS) / sizeof(TIMER_PRESETS[0]))

// #endregion

// #region Core Types

//! Gap buffer for efficient text editing
//! @see dawn_gap.h for operations
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t gap_start;
    size_t gap_end;
} GapBuffer;

//! Application mode/screen
typedef enum {
    MODE_WELCOME,       //!< Start screen
    MODE_WRITING,       //!< Main editor
    MODE_TIMER_SELECT,  //!< Timer configuration
    MODE_HISTORY,       //!< Document history browser
    MODE_STYLE,         //!< Style selection (unused)
    MODE_FINISHED,      //!< Timer completed screen
    MODE_TITLE_EDIT,    //!< Document title editor (modal)
    MODE_HELP,          //!< Keyboard shortcuts help
    MODE_IMAGE_EDIT,    //!< Image dimension editor (modal)
    MODE_TOC,           //!< Table of contents navigation (modal)
    MODE_SEARCH         //!< Document search (modal)
} AppMode;

//! Push a modal mode (saves current mode for later restoration)
#define MODE_PUSH(new_mode) do { app.prev_mode = app.mode; app.mode = (new_mode); } while(0)

//! Pop back to previous mode
#define MODE_POP() do { app.mode = app.prev_mode; } while(0)

//! Color theme
typedef enum {
    THEME_LIGHT,
    THEME_DARK
} Theme;

//! Writing style (visual presentation)
typedef enum {
    STYLE_MINIMAL,      //!< Clean, minimal UI
    STYLE_TYPEWRITER,   //!< Monospace feel
    STYLE_ELEGANT       //!< Italic, refined
} WritingStyle;

//! AI chat message
typedef struct {
    char *text;
    size_t len;
    bool is_user;       //!< true = user message, false = AI response
} ChatMessage;

//! History entry for saved documents
typedef struct {
    char *path;         //!< Full path to .md file
    char *title;        //!< Document title from frontmatter
    char *date_str;     //!< Formatted modification date
} HistoryEntry;

// #endregion

// #region Key Codes (Platform-Independent Mapping)

//! Extended key codes for special keys
//! These map to platform key codes
enum {
    KEY_NONE = PLATFORM_KEY_NONE,
    KEY_ESC = 0x1b,

    // Arrow keys
    KEY_UP = PLATFORM_KEY_UP,
    KEY_DOWN = PLATFORM_KEY_DOWN,
    KEY_RIGHT = PLATFORM_KEY_RIGHT,
    KEY_LEFT = PLATFORM_KEY_LEFT,

    // Navigation
    KEY_HOME = PLATFORM_KEY_HOME,
    KEY_END = PLATFORM_KEY_END,
    KEY_PGUP = PLATFORM_KEY_PGUP,
    KEY_PGDN = PLATFORM_KEY_PGDN,
    KEY_DEL = PLATFORM_KEY_DEL,

    // Shift+Arrow (selection)
    KEY_SHIFT_UP = PLATFORM_KEY_SHIFT_UP,
    KEY_SHIFT_DOWN = PLATFORM_KEY_SHIFT_DOWN,
    KEY_SHIFT_LEFT = PLATFORM_KEY_SHIFT_LEFT,
    KEY_SHIFT_RIGHT = PLATFORM_KEY_SHIFT_RIGHT,

    // Ctrl+Arrow (word movement)
    KEY_CTRL_LEFT = PLATFORM_KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT = PLATFORM_KEY_CTRL_RIGHT,
    KEY_CTRL_SHIFT_LEFT = PLATFORM_KEY_CTRL_SHIFT_LEFT,
    KEY_CTRL_SHIFT_RIGHT = PLATFORM_KEY_CTRL_SHIFT_RIGHT,

    // Alt+Arrow (word movement on macOS)
    KEY_ALT_LEFT = PLATFORM_KEY_ALT_LEFT,
    KEY_ALT_RIGHT = PLATFORM_KEY_ALT_RIGHT,
    KEY_ALT_SHIFT_LEFT = PLATFORM_KEY_ALT_SHIFT_LEFT,
    KEY_ALT_SHIFT_RIGHT = PLATFORM_KEY_ALT_SHIFT_RIGHT,

    // Alt+Up/Down (half-screen movement)
    KEY_ALT_UP = PLATFORM_KEY_ALT_UP,
    KEY_ALT_DOWN = PLATFORM_KEY_ALT_DOWN,

    // Ctrl+Home/End (document start/end)
    KEY_CTRL_HOME = PLATFORM_KEY_CTRL_HOME,
    KEY_CTRL_END = PLATFORM_KEY_CTRL_END,

    // Mouse
    KEY_MOUSE_SCROLL_UP = PLATFORM_KEY_MOUSE_SCROLL_UP,
    KEY_MOUSE_SCROLL_DOWN = PLATFORM_KEY_MOUSE_SCROLL_DOWN,
    KEY_MOUSE_CLICK = PLATFORM_KEY_MOUSE_CLICK,

    // Special
    KEY_BTAB = PLATFORM_KEY_BTAB  //!< Shift+Tab (backtab)
};

// #endregion

// #region Application State

//! Global application state
typedef struct {
    // Document
    GapBuffer text;     //!< Document content
    size_t cursor;      //!< Cursor position in text

    // Selection
    bool selecting;     //!< Selection mode active
    size_t sel_anchor;  //!< Selection start position

    // Viewport
    int scroll_y;       //!< Vertical scroll offset

    // Timer
    int timer_mins;     //!< Timer duration in minutes
    int64_t timer_start; //!< Timer start timestamp
    int64_t timer_paused_at; //!< Pause timestamp
    bool timer_on;      //!< Timer running
    bool timer_paused;  //!< Timer paused
    bool timer_done;    //!< Timer completed

    // UI State
    AppMode mode;       //!< Current screen/mode
    AppMode prev_mode;  //!< Previous mode (for modal return)
    Theme theme;        //!< Color theme
    WritingStyle style; //!< Writing style
    int preset_idx;     //!< Selected timer preset index
    bool focus_mode;    //!< Focus mode enabled
    bool plain_mode;    //!< Plain text mode (no WYSIWYG rendering)
    bool preview_mode;  //!< Read-only preview mode

    // Display
    int rows, cols;     //!< Display dimensions

    // History
    HistoryEntry *history;  //!< Document history array
    int hist_count;         //!< Number of history entries
    int hist_sel;           //!< Selected history index

    // Current Session
    char *session_path;     //!< Path to current document
    char *session_title;    //!< Document title
    char title_edit_buf[256]; //!< Title edit buffer
    size_t title_edit_len;    //!< Title edit length
    size_t title_edit_cursor; //!< Title edit cursor

    // Image Edit
    size_t img_edit_pos;      //!< Position of image being edited
    size_t img_edit_total_len; //!< Total length of image syntax
    char img_edit_width_buf[16];  //!< Width edit buffer
    char img_edit_height_buf[16]; //!< Height edit buffer
    size_t img_edit_width_len;    //!< Width buffer length
    size_t img_edit_height_len;   //!< Height buffer length
    int img_edit_field;       //!< 0 = width, 1 = height
    bool img_edit_width_pct;  //!< Width is percentage
    bool img_edit_height_pct; //!< Height is percentage

    // AI Chat
    bool ai_open;           //!< AI panel visible
    bool ai_focused;        //!< AI input focused
    char ai_input[MAX_AI_INPUT]; //!< AI input buffer
    size_t ai_input_len;    //!< AI input length
    size_t ai_input_cursor; //!< AI input cursor
    ChatMessage *chat_msgs; //!< Chat history
    int chat_count;         //!< Number of messages
    int chat_scroll;        //!< Chat scroll offset
    bool ai_thinking;       //!< AI processing request

    #if HAS_LIBAI
    ai_context_t *ai_ctx;       //!< libai context
    ai_session_id_t ai_session; //!< libai session
    #endif
    bool ai_ready;          //!< AI available

    // Undo/Redo
    #define MAX_UNDO 100
    struct {
        char *text;         //!< Saved text state
        size_t text_len;    //!< Length of saved text
        size_t cursor;      //!< Cursor position
    } undo_stack[MAX_UNDO];
    int undo_count;         //!< Number of undo states
    int undo_pos;           //!< Current position in undo stack

    // State flags
    bool resize_needed;     //!< Display resize pending
    bool quit;              //!< Quit requested
    bool hide_cursor_syntax;//!< When true, don't show raw syntax when cursor is over markdown elements

    // Auto-save
    int64_t last_save_time;  //!< Last auto-save timestamp

    // Block cache (forward declared, allocated on demand)
    void *block_cache;      //!< BlockCache* - block-based document model

    // Syntax highlighting
    void *hl_ctx;           //!< hl_ctx_t* - syntax highlight context

    // TOC and Search (forward declared, allocated on demand)
    void *toc_state;        //!< TocState* - table of contents state
    void *search_state;     //!< SearchState* - document search state
} App;

//! Global application instance
extern App app;

// #endregion

#endif // DAWN_TYPES_H
