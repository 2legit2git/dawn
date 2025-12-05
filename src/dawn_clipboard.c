// dawn_clipboard.c

#include "dawn_clipboard.h"

// #region Clipboard Operations

void clipboard_copy(const char *text, size_t len) {
    const PlatformBackend *p = platform_get();
    if (p && p->clipboard_copy) {
        p->clipboard_copy(text, len);
    }
}

char *clipboard_paste(size_t *out_len) {
    const PlatformBackend *p = platform_get();
    if (p && p->clipboard_paste) {
        return p->clipboard_paste(out_len);
    }
    *out_len = 0;
    return NULL;
}

// #endregion
