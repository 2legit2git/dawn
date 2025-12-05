// dawn_wrap.h

#ifndef DAWN_WRAP_H
#define DAWN_WRAP_H

#include "dawn_types.h"

// #region Constants

//! Default tab size in spaces
#define WRAP_DEFAULT_TAB_SIZE 4

//! Non-breaking space (U+00A0)
#define NBSP 0x00A0

// #endregion

// #region Types

//! Configuration options for word wrapping
typedef struct {
    int tab_size;           //!< Number of spaces per tab (default: 4)
    bool trim_whitespace;   //!< Trim leading/trailing whitespace on lines
    bool split_words;       //!< Allow splitting words with hyphen if too long
    bool keep_dash_with_word; //!< Keep dashes attached to preceding word
} WrapConfig;

//! A single wrapped line segment
typedef struct {
    size_t start;           //!< Byte offset in source text
    size_t end;             //!< Byte offset end (exclusive)
    int display_width;      //!< Display width in terminal columns
    int segment_in_orig;    //!< Which segment this is within original line
    bool is_hard_break;     //!< True if line ended with actual newline
    bool ends_with_split;   //!< True if word was split with hyphen
} WrapLine;

//! Result of wrapping text into lines
typedef struct {
    WrapLine *lines;        //!< Array of wrapped lines
    int count;              //!< Number of lines
    int capacity;           //!< Allocated capacity
    WrapConfig config;      //!< Configuration used for wrapping
    int limit;              //!< Width limit used
} WrapResult;

// #endregion

// #region Configuration

//! Get default wrap configuration
//! @return default configuration with sensible defaults
WrapConfig wrap_config_default(void);

// #endregion

// #region Wrap Result Management

//! Initialize a wrap result structure
//! @param wr wrap result to initialize
void wrap_init(WrapResult *wr);

//! Free wrap result memory
//! @param wr wrap result to free
void wrap_free(WrapResult *wr);

// #endregion

// #region Text Wrapping

//! Wrap text from gap buffer to fit given width
//! @param gb gap buffer containing text
//! @param width maximum display width in columns
//! @param out wrap result structure (must be initialized)
//! @return number of wrapped lines
int wrap_text(const GapBuffer *gb, int width, WrapResult *out);

//! Wrap text from gap buffer with configuration options
//! @param gb gap buffer containing text
//! @param width maximum display width in columns
//! @param config wrapping configuration
//! @param out wrap result structure (must be initialized)
//! @return number of wrapped lines
int wrap_text_config(const GapBuffer *gb, int width, WrapConfig config, WrapResult *out);

//! Wrap a plain string to fit given width
//! @param text string to wrap
//! @param len length of string in bytes
//! @param width maximum display width in columns
//! @param out wrap result structure (must be initialized)
//! @return number of wrapped lines
int wrap_string(const char *text, size_t len, int width, WrapResult *out);

//! Wrap a plain string with configuration options
//! @param text string to wrap
//! @param len length of string in bytes
//! @param width maximum display width in columns
//! @param config wrapping configuration
//! @param out wrap result structure (must be initialized)
//! @return number of wrapped lines
int wrap_string_config(const char *text, size_t len, int width, WrapConfig config, WrapResult *out);

// #endregion

// #region Display Width

//! Get display width of a UTF-8 string
//! @param text UTF-8 string
//! @param len length in bytes
//! @return display width in terminal columns
int utf8_display_width(const char *text, size_t len);

//! Get display width of text range in gap buffer
//! @param gb gap buffer to measure
//! @param start start byte position
//! @param end end byte position
//! @return display width in terminal columns
int gap_display_width(const GapBuffer *gb, size_t start, size_t end);

//! Find the best wrap point within a range of text
//! @param gb gap buffer to search
//! @param start start byte position
//! @param end end byte position (or SIZE_MAX for no limit)
//! @param width maximum display width
//! @param out_width output: actual width at wrap point (optional)
//! @return byte position to wrap at (after last space/dash if possible)
size_t gap_find_wrap_point(const GapBuffer *gb, size_t start, size_t end, int width, int *out_width);

// #endregion

// #region Grapheme Navigation

//! Move to next grapheme cluster boundary in gap buffer
//! @param gb gap buffer to navigate
//! @param pos current byte position
//! @return byte position of next grapheme start
size_t gap_grapheme_next(const GapBuffer *gb, size_t pos);

//! Move to previous grapheme cluster boundary in gap buffer
//! @param gb gap buffer to navigate
//! @param pos current byte position
//! @return byte position of previous grapheme start
size_t gap_grapheme_prev(const GapBuffer *gb, size_t pos);

//! Get display width of grapheme at position
//! @param gb gap buffer to query
//! @param pos byte position of grapheme
//! @param next_pos output: position after this grapheme (optional)
//! @return display width in terminal columns
int gap_grapheme_width(const GapBuffer *gb, size_t pos, size_t *next_pos);

// #endregion

// #region Grapheme Utilities

//! Check if grapheme is "wordy" (letter or number)
//! @param text pointer to start of grapheme cluster
//! @param len length of grapheme in bytes
//! @return true if grapheme starts with letter or number
bool grapheme_is_wordy(const char *text, size_t len);

//! Check if codepoint is a word-break character (space, tab, dash)
//! @param cp Unicode codepoint
//! @return true if character can be a word break point
bool is_break_char(utf8proc_int32_t cp);

// #endregion

#endif // DAWN_WRAP_H
