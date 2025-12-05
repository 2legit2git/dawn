// dawn_args.h

#ifndef DAWN_ARGS_H
#define DAWN_ARGS_H

#include <stdbool.h>
#include <stddef.h>

// #region Argument Types

//! Parsed command-line arguments
typedef struct {
    // File to open (positional or --file/-f)
    char *file;             //!< Path to file to open (copied to .dawn)

    // Demo mode (--demo/-d)
    bool demo_mode;         //!< Demo mode - replay document typing
    char *demo_file;        //!< File to replay in demo mode

    // Render mode (--render/-r or stdin pipe)
    bool render_mode;       //!< Render input to stdout and exit

    // Preview mode (--preview/-p)
    bool preview_mode;      //!< Read-only preview of file

    // Theme (--theme/-t)
    int theme;              //!< Theme: -1 = not set, 0 = light, 1 = dark

    // Help/Version
    bool show_help;         //!< Show help and exit
    bool show_version;      //!< Show version and exit

    // Error handling
    bool error;             //!< Parsing error occurred
    const char *error_msg;  //!< Error message
} DawnArgs;

// #endregion

// #region Functions

//! Parse command-line arguments
//! @param argc argument count from main
//! @param argv argument vector from main
//! @return parsed arguments structure
DawnArgs args_parse(int argc, char *argv[]);

//! Free resources allocated by args_parse
//! @param args pointer to arguments structure
void args_free(DawnArgs *args);

//! Copy a file to the .dawn directory
//! @param src_path source file path
//! @param out_path buffer for destination path
//! @param out_size size of output buffer
//! @return true on success
bool args_copy_to_dawn(const char *src_path, char *out_path, size_t out_size);

//! Print usage information to stderr
void args_print_usage(const char *program_name);

//! Print version information to stdout
void args_print_version(void);

//! Check if stdin has data (for pipe detection)
//! @return true if stdin is a pipe with data
bool args_stdin_has_data(void);

// #endregion

#endif // DAWN_ARGS_H
