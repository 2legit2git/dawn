// main_term.c - Terminal Frontend for Dawn
//! POSIX terminal implementation that uses the Dawn engine
//! This file handles platform initialization and the main loop

#include "dawn_app.h"
#include "dawn_args.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    // Parse command-line arguments
    DawnArgs args = args_parse(argc, argv);

    // Handle errors
    if (args.error) {
        fprintf(stderr, "dawn: %s\n", args.error_msg);
        args_print_usage(argv[0]);
        args_free(&args);
        return 1;
    }

    // Handle help/version (no platform needed)
    if (args.show_help) {
        args_print_usage(argv[0]);
        args_free(&args);
        return 0;
    }

    if (args.show_version) {
        args_print_version();
        args_free(&args);
        return 0;
    }

    // Initialize platform backend
    if (!platform_init(&platform_posix)) {
        fprintf(stderr, "dawn: failed to initialize platform\n");
        args_free(&args);
        return 1;
    }

    // Determine theme (command-line overrides default)
    Theme theme = (args.theme >= 0) ? (Theme)args.theme : THEME_DARK;

    // Initialize Dawn engine
    if (!dawn_engine_init(theme)) {
        fprintf(stderr, "dawn: failed to initialize engine\n");
        platform_shutdown();
        args_free(&args);
        return 1;
    }

    // Load file if specified
    if (args.file) {
        if (args.preview_mode) {
            // Preview mode: load directly without copying
            if (!dawn_preview_document(args.file)) {
                fprintf(stderr, "dawn: cannot open file: %s\n", args.file);
                dawn_engine_shutdown();
                platform_shutdown();
                args_free(&args);
                return 1;
            }
        } else {
            // Edit mode: copy to .dawn directory
            char dest_path[512];
            if (args_copy_to_dawn(args.file, dest_path, sizeof(dest_path))) {
                dawn_load_document(dest_path);
            } else {
                fprintf(stderr, "dawn: cannot open file: %s\n", args.file);
                dawn_engine_shutdown();
                platform_shutdown();
                args_free(&args);
                return 1;
            }
        }
    }

    args_free(&args);

    // Main loop
    const PlatformBackend *p = platform_get();
    while (dawn_frame()) {
        if (p && p->input_available) {
            p->input_available(6.944f);
        }
        if (p && p->execute_pending_jobs) {
            p->execute_pending_jobs();
        }
    }

    // Cleanup
    dawn_engine_shutdown();
    platform_shutdown();

    return 0;
}
