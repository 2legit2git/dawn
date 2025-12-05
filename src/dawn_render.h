// dawn_render.h

#ifndef DAWN_RENDER_H
#define DAWN_RENDER_H

#include "dawn_types.h"

// #region Utility Functions

//! Clear entire screen with background color
void render_clear(void);

//! Print centered text at given row
//! @param row display row (1-based)
//! @param text text to display
//! @param fg foreground color
void render_center_text(int row, const char *text, Color fg);

//! Render a floating popup box centered on screen
//! @param width box width in columns
//! @param height box height in rows
//! @param out_top output: top-left row (1-based)
//! @param out_left output: top-left column (1-based)
void render_popup_box(int width, int height, int *out_top, int *out_left);

// #endregion

// #region Screen Renderers

//! Render the welcome/menu screen
void render_welcome(void);

//! Render the timer selection screen
void render_timer_select(void);

//! Render the style selection screen
void render_style_select(void);

//! Render the help screen with keyboard shortcuts
void render_help(void);

//! Render the session history browser
void render_history(void);

//! Render the session completion screen
void render_finished(void);

//! Render the title editing screen
void render_title_edit(void);

//! Render the image dimension editing screen
void render_image_edit(void);

//! Render the table of contents overlay
void render_toc(void);

//! Render the search overlay
void render_search(void);

// #endregion

#endif // DAWN_RENDER_H
