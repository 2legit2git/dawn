// dawn_input.c

#include "dawn_input.h"

// #region Input Reading

int input_read_key(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->read_key) {
        return p->read_key();
    }
    return KEY_NONE;
}

int input_last_mouse_col(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->get_last_mouse_col) {
        return p->get_last_mouse_col();
    }
    return 0;
}

int input_last_mouse_row(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->get_last_mouse_row) {
        return p->get_last_mouse_row();
    }
    return 0;
}

// #endregion
