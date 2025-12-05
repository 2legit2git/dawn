// dawn_image.c

#include "dawn_image.h"
#include "dawn_gap.h"
#include <string.h>

bool image_is_supported(const char *path) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_is_supported) {
        return p->image_is_supported(path);
    }
    return false;
}

int image_display_at(const char *path, int row, int col, int max_cols, int max_rows) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_display) {
        return p->image_display(path, row, col, max_cols, max_rows);
    }
    return 0;
}

int image_display_at_cropped(const char *path, int row, int col, int max_cols,
                               int crop_top_rows, int visible_rows) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_display_cropped) {
        return p->image_display_cropped(path, row, col, max_cols, crop_top_rows, visible_rows);
    }
    return 0;
}

int image_display(const char *path, int max_cols, int max_rows) {
    return image_display_at(path, 0, 0, max_cols, max_rows);
}

void image_frame_start(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_frame_start) {
        p->image_frame_start();
    }
}

void image_frame_end(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_frame_end) {
        p->image_frame_end();
    }
}

void image_mask_region(int col, int row, int cols, int rows, Color bg) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_mask_region) {
        PlatformColor pc = {bg.r, bg.g, bg.b};
        p->image_mask_region(col, row, cols, rows, pc);
    }
}

void image_clear_all(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_clear_all) {
        p->image_clear_all();
    }
}

void image_cache_invalidate(const char *path) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_invalidate) {
        p->image_invalidate(path);
    }
}

bool image_get_size(const char *path, int *width, int *height) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_get_size) {
        return p->image_get_size(path, width, height);
    }
    return false;
}

int image_calc_rows(int pixel_width, int pixel_height, int max_cols, int max_rows) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_calc_rows) {
        return p->image_calc_rows(pixel_width, pixel_height, max_cols, max_rows);
    }
    // Fallback calculation
    if (pixel_width <= 0 || pixel_height <= 0) return 1;
    if (max_rows > 0) return max_rows;
    if (max_cols <= 0) max_cols = 40;
    int rows = (int)((double)max_cols * ((double)pixel_height / (double)pixel_width) * 0.5 + 0.5);
    return rows > 0 ? rows : 1;
}

bool image_resolve_and_cache_to(const char *raw_path, const char *base_dir, char *out, size_t out_size) {
    const PlatformBackend *p = platform_get();
    if (p && p->image_resolve_path) {
        return p->image_resolve_path(raw_path, base_dir, out, out_size);
    }
    return false;
}

