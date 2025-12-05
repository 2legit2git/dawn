// dawn_args.c

#define _POSIX_C_SOURCE 200809L

#include "dawn_args.h"
#include "dawn_types.h"

#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <libgen.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>

// #region Option Definitions

static const char *short_opts = "f:d:t:p:rhv";

static struct option long_opts[] = {
    {"file",    required_argument, NULL, 'f'},
    {"demo",    required_argument, NULL, 'd'},
    {"theme",   required_argument, NULL, 't'},
    {"preview", required_argument, NULL, 'p'},
    {"render",  no_argument,       NULL, 'r'},
    {"help",    no_argument,       NULL, 'h'},
    {"version", no_argument,       NULL, 'v'},
    {NULL,      0,                 NULL,  0 }
};

// #endregion

// #region Helper Functions

//! Resolve path to absolute
//! @param path input path
//! @return newly allocated absolute path
static char *resolve_path(const char *path) {
    if (!path) return NULL;

    // Already absolute
    if (path[0] == '/') {
        return strdup(path);
    }

    // Home directory expansion
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            size_t len = strlen(home) + strlen(path);  // path includes ~
            char *result = malloc(len);
            if (result) {
                snprintf(result, len, "%s%s", home, path + 1);
            }
            return result;
        }
    }

    // Relative path - resolve from cwd
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        size_t len = strlen(cwd) + 1 + strlen(path) + 1;
        char *result = malloc(len);
        if (result) {
            snprintf(result, len, "%s/%s", cwd, path);
        }
        return result;
    }

    return strdup(path);
}

//! Parse theme argument
//! @param arg theme string ("light" or "dark")
//! @return 0 for light, 1 for dark, -1 for invalid
static int parse_theme(const char *arg) {
    if (!arg) return -1;
    if (strcasecmp(arg, "light") == 0 || strcmp(arg, "0") == 0) return 0;
    if (strcasecmp(arg, "dark") == 0 || strcmp(arg, "1") == 0) return 1;
    return -1;
}

// #endregion

// #region Public Functions

DawnArgs args_parse(int argc, char *argv[]) {
    DawnArgs args = {0};
    args.theme = -1;  // Not set

    // Reset getopt
    optind = 1;
    opterr = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
        switch (opt) {
            case 'f':
                args.file = resolve_path(optarg);
                break;

            case 'd':
                args.demo_mode = true;
                args.demo_file = resolve_path(optarg);
                break;

            case 't':
                args.theme = parse_theme(optarg);
                if (args.theme < 0) {
                    args.error = true;
                    args.error_msg = "Invalid theme (use 'light' or 'dark')";
                }
                break;

            case 'p':
                args.preview_mode = true;
                args.file = resolve_path(optarg);
                break;

            case 'r':
                args.render_mode = true;
                break;

            case 'h':
                args.show_help = true;
                break;

            case 'v':
                args.show_version = true;
                break;

            case '?':
                args.error = true;
                args.error_msg = "Unknown option";
                break;

            case ':':
                args.error = true;
                args.error_msg = "Missing argument";
                break;
        }
    }

    // Check for positional argument (file path without -f)
    if (optind < argc && !args.file && !args.demo_file) {
        args.file = resolve_path(argv[optind]);
    }

    // Check for stdin data if render mode or if stdin is a pipe
    if (!args.file && !args.demo_mode && !args.show_help && !args.show_version) {
        if (args_stdin_has_data()) {
            args.render_mode = true;
        }
    }

    // Validate combinations
    if (args.demo_mode && args.preview_mode) {
        args.error = true;
        args.error_msg = "Cannot use --demo and --preview together";
    }

    if (args.render_mode && (args.file || args.demo_mode || args.preview_mode)) {
        args.error = true;
        args.error_msg = "Cannot use --render with --file, --demo, or --preview";
    }

    if (args.preview_mode && !args.file) {
        args.error = true;
        args.error_msg = "--preview requires a file path";
    }

    return args;
}

void args_free(DawnArgs *args) {
    if (!args) return;
    free(args->file);
    free(args->demo_file);
    args->file = NULL;
    args->demo_file = NULL;
}

bool args_copy_to_dawn(const char *src_path, char *out_path, size_t out_size) {
    if (!src_path || !out_path || out_size == 0) return false;

    // Get .dawn directory
    const char *home = getenv("HOME");
    if (!home) return false;

    char dawn_dir[512];
    snprintf(dawn_dir, sizeof(dawn_dir), "%s/%s", home, HISTORY_DIR_NAME);

    // Check if file is already in .dawn directory - if so, just use it directly
    if (strncmp(src_path, dawn_dir, strlen(dawn_dir)) == 0) {
        strncpy(out_path, src_path, out_size - 1);
        out_path[out_size - 1] = '\0';
        return true;
    }

    // Ensure .dawn directory exists
    mkdir(dawn_dir, 0755);

    // Generate unique filename based on original name and timestamp
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;

    // Remove .md extension if present for cleaner naming
    char name[256];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *ext = strrchr(name, '.');
    if (ext && strcasecmp(ext, ".md") == 0) {
        *ext = '\0';
    }

    // Create destination path with timestamp
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(out_path, out_size, "%s/%04d-%02d-%02d_%02d%02d%02d_%s.md",
             dawn_dir, t->tm_year+1900, t->tm_mon+1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, name);

    // Copy file contents
    FILE *src = fopen(src_path, "r");
    if (!src) return false;

    FILE *dst = fopen(out_path, "w");
    if (!dst) {
        fclose(src);
        return false;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }

    fclose(src);
    fclose(dst);
    return true;
}

void args_print_usage(const char *program_name) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [FILE]\n"
        "\n"
        "Dawn: Draft Anything, Write Now\n"
        "A distraction-free writing environment with live markdown rendering\n"
        "\n"
        "Options:\n"
        "  -f, --file FILE     Open FILE (copies to ~/.dawn for editing)\n"
        "  -p, --preview FILE  Preview FILE in read-only mode\n"
        "  -d, --demo FILE     Demo mode: replay FILE as if being typed\n"
        "  -t, --theme THEME   Set theme: 'light' or 'dark'\n"
        "  -r, --render        Render stdin to stdout and exit\n"
        "  -h, --help          Show this help message\n"
        "  -v, --version       Show version information\n"
        "\n"
        "Arguments:\n"
        "  FILE                Path to markdown file (same as --file FILE)\n"
        "\n"
        "Examples:\n"
        "  %s                       Start with welcome screen\n"
        "  %s notes.md              Open notes.md (copied to ~/.dawn)\n"
        "  %s -p README.md          Preview README.md (read-only)\n"
        "  %s -t light              Start with light theme\n"
        "  %s -d demo.md -t dark    Demo with dark theme\n"
        "  cat doc.md | %s -r       Render markdown to terminal\n"
        "\n",
        program_name, program_name, program_name, program_name, program_name, program_name, program_name);
}

void args_print_version(void) {
    printf("%s %s\n", APP_NAME, VERSION);
    printf("%s\n", APP_TAGLINE);
}

bool args_stdin_has_data(void) {
    // Check if stdin is a terminal
    if (isatty(STDIN_FILENO)) {
        return false;
    }

    // Check if there's data available
    fd_set fds;
    struct timeval tv = {0, 0};

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

// #endregion
