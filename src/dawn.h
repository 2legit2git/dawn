// dawn.h

#ifndef DAWN_H
#define DAWN_H

#include "dawn_types.h"

//! Initialize the Dawn application
//! @param theme initial theme (THEME_LIGHT or THEME_DARK)
//! @return true on success
bool dawn_init(Theme theme);

//! Load a file for editing
//! @param path path to file
//! @return true on success
bool dawn_load_file(const char* path);

//! Run one iteration of the main loop
//! @return true to continue, false to quit
bool dawn_update(void);

//! Shutdown the Dawn application
void dawn_shutdown(void);

//! Handle window resize signal
void dawn_on_resize(void);

//! Handle quit signal (SIGINT, SIGTERM, etc.)
void dawn_on_quit(void);

//! Global application state (defined in dawn.c)
extern App app;

#endif // DAWN_H
