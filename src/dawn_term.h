// dawn_term.h
//! Wraps platform abstraction for convenience

#ifndef DAWN_TERM_H
#define DAWN_TERM_H

#include "dawn_types.h"

// #region Display Lifecycle

//! Initialize display system
//! Must be called before any other display operations
//! @return true on success
bool term_init(void);

//! Cleanup display system
//! Must be called before program exit
void term_cleanup(void);

// #endregion

// #region Display State

//! Query and update display dimensions (rows/cols)
//! Results stored in app.rows and app.cols
void term_get_size(void);

//! Check if display resize occurred
//! @return true if resize occurred since last check
bool term_check_resize(void);

//! Check if quit was requested
//! @return true if quit requested
bool term_check_quit(void);

// #endregion

// #region Terminal Capabilities

//! Check if platform has a specific capability
//! @param cap capability to check (PLATFORM_CAP_*)
//! @return true if capability is available
#define TERM_HAS(cap) platform_has(cap)

//! Convenience capability check macros
#define TERM_CAP_TRUE_COLOR      PLATFORM_CAP_TRUE_COLOR
#define TERM_CAP_SYNC_OUTPUT     PLATFORM_CAP_SYNC_OUTPUT
#define TERM_CAP_STYLED_UNDERLINE PLATFORM_CAP_STYLED_UNDERLINE
#define TERM_CAP_TEXT_SIZING     PLATFORM_CAP_TEXT_SIZING
#define TERM_CAP_KITTY_GRAPHICS  PLATFORM_CAP_IMAGES
#define TERM_CAP_KITTY_KEYBOARD  PLATFORM_CAP_STYLED_UNDERLINE  // Implies Kitty
#define TERM_CAP_BRACKETED_PASTE PLATFORM_CAP_BRACKETED_PASTE
#define TERM_CAP_FOCUS_EVENTS    PLATFORM_CAP_FOCUS_EVENTS

// #endregion

#endif // DAWN_TERM_H
