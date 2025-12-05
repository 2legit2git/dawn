// dawn_term.c

#include "dawn_term.h"

// #region Display Lifecycle

bool term_init(void) {
    // Initialize with POSIX backend
    if (!platform_init(&platform_posix)) {
        return false;
    }

    // Get initial size
    term_get_size();
    return true;
}

void term_cleanup(void) {
    platform_shutdown();
}

// #endregion

// #region Display State

void term_get_size(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->get_size) {
        p->get_size(&app.cols, &app.rows);
    }
}

bool term_check_resize(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->check_resize) {
        return p->check_resize();
    }
    return false;
}

bool term_check_quit(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->check_quit) {
        return p->check_quit();
    }
    return false;
}

// #endregion
