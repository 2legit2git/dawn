// main_web.c - Web Frontend for Dawn
//! Emscripten/Canvas implementation that uses the Dawn engine
//! This file handles platform initialization and the main loop via requestAnimationFrame

#ifdef __EMSCRIPTEN__

#include "dawn_app.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <emscripten.h>
#include <emscripten/html5.h>

// External declaration of the web platform backend
extern const PlatformBackend platform_web;

// Frame callback for requestAnimationFrame loop
static void main_loop(void) {
    if (!dawn_frame()) {
        // App wants to quit
        emscripten_cancel_main_loop();
        dawn_engine_shutdown();
        platform_shutdown();
    }
}

// Called from JavaScript to load a file
EMSCRIPTEN_KEEPALIVE
void dawn_web_load_file(const char *content, size_t len, const char *filename) {
    // Save to virtual filesystem and load
    char path[256];
    snprintf(path, sizeof(path), "/dawn/%s", filename);

    // Write content to virtual FS
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(content, 1, len, f);
        fclose(f);
        dawn_load_document(path);
    }
}

// Called from JavaScript to create a new document
EMSCRIPTEN_KEEPALIVE
void dawn_web_new_document(void) {
    dawn_new_document();
}

// Called from JavaScript to save the current document
EMSCRIPTEN_KEEPALIVE
void dawn_web_save(void) {
    dawn_save_document();
}

// Called from JavaScript to set the theme
EMSCRIPTEN_KEEPALIVE
void dawn_web_set_theme(int dark) {
    // Theme is set at init, would need to add a runtime theme change API
    (void)dark;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Initialize platform backend
    if (!platform_init(&platform_web)) {
        fprintf(stderr, "dawn: failed to initialize platform\n");
        return 1;
    }

    // Initialize Dawn engine with dark theme
    if (!dawn_engine_init(THEME_DARK)) {
        fprintf(stderr, "dawn: failed to initialize engine\n");
        platform_shutdown();
        return 1;
    }

    // Start the main loop using requestAnimationFrame
    // 0 = use requestAnimationFrame (vsync)
    // 1 = simulate infinite loop (not recommended)
    emscripten_set_main_loop(main_loop, 0, 1);

    // This is never reached because emscripten_set_main_loop doesn't return
    // when the last parameter is 1 (simulate infinite loop)

    return 0;
}

#endif // __EMSCRIPTEN__
