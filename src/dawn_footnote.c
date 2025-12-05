// dawn_footnote.c

#include "dawn_footnote.h"
#include "dawn_md.h"
#include <string.h>
#include <stdlib.h>

// #region Types

//! Footnote tracking for navigation
typedef struct {
    char id[64];       //!< Footnote identifier
    size_t ref_pos;    //!< Position of first reference
    size_t def_pos;    //!< Position of definition, or SIZE_MAX if none
} FootnoteInfo;

// #endregion

// #region Internal Helpers

//! Scan document for all footnote references and definitions
//! @param gb gap buffer to scan
//! @param count output: number of footnotes found
//! @return array of FootnoteInfo (caller must free)
static FootnoteInfo *scan_footnotes(GapBuffer *gb, int *count) {
    *count = 0;
    size_t len = gap_len(gb);
    FootnoteInfo *notes = NULL;
    int capacity = 0;

    // First pass: find all references
    for (size_t pos = 0; pos < len; pos++) {
        size_t id_start, id_len, total;
        if (md_check_footnote_ref(gb, pos, &id_start, &id_len, &total)) {
            // Extract ID
            char id[64];
            size_t idl = id_len < sizeof(id) - 1 ? id_len : sizeof(id) - 1;
            for (size_t i = 0; i < idl; i++) {
                id[i] = gap_at(gb, id_start + i);
            }
            id[idl] = '\0';

            // Check if we already have this ID
            bool found = false;
            for (int i = 0; i < *count; i++) {
                if (strcmp(notes[i].id, id) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (*count >= capacity) {
                    capacity = capacity == 0 ? 8 : capacity * 2;
                    notes = realloc(notes, sizeof(FootnoteInfo) * (size_t)capacity);
                }
                strncpy(notes[*count].id, id, sizeof(notes[*count].id) - 1);
                notes[*count].id[sizeof(notes[*count].id) - 1] = '\0';
                notes[*count].ref_pos = pos;
                notes[*count].def_pos = SIZE_MAX;
                (*count)++;
            }

            pos += total - 1;
        }
    }

    // Second pass: find definitions
    for (size_t pos = 0; pos < len; pos++) {
        size_t id_start, id_len, content_start, total;
        if (md_check_footnote_def(gb, pos, &id_start, &id_len, &content_start, &total)) {
            char id[64];
            size_t idl = id_len < sizeof(id) - 1 ? id_len : sizeof(id) - 1;
            for (size_t i = 0; i < idl; i++) {
                id[i] = gap_at(gb, id_start + i);
            }
            id[idl] = '\0';

            // Match to references
            for (int i = 0; i < *count; i++) {
                if (strcmp(notes[i].id, id) == 0) {
                    notes[i].def_pos = pos;
                    break;
                }
            }
        }
    }

    return notes;
}

//! Create missing footnote definitions at end of document
//! @param gb gap buffer to modify
//! @return position of first new definition, or SIZE_MAX if none created
static size_t create_missing_footnotes(GapBuffer *gb) {
    int count;
    FootnoteInfo *notes = scan_footnotes(gb, &count);
    if (!notes || count == 0) {
        free(notes);
        return SIZE_MAX;
    }

    // Find which ones are missing
    int missing = 0;
    for (int i = 0; i < count; i++) {
        if (notes[i].def_pos == SIZE_MAX) missing++;
    }

    if (missing == 0) {
        free(notes);
        return SIZE_MAX;
    }

    size_t len = gap_len(gb);
    size_t insert_pos = len;
    size_t first_new = SIZE_MAX;

    // Make sure there's a blank line before footnotes
    if (len > 0 && gap_at(gb, len - 1) != '\n') {
        gap_insert(gb, insert_pos, '\n');
        insert_pos++;
    }
    if (len > 1 && gap_at(gb, len - 2) != '\n') {
        gap_insert(gb, insert_pos, '\n');
        insert_pos++;
    }

    // Add separator
    const char *sep = "---\n\n";
    for (const char *p = sep; *p; p++) {
        gap_insert(gb, insert_pos, *p);
        insert_pos++;
    }

    // Add missing definitions
    for (int i = 0; i < count; i++) {
        if (notes[i].def_pos == SIZE_MAX) {
            if (first_new == SIZE_MAX) {
                first_new = insert_pos;
            }

            // [^id]:
            gap_insert(gb, insert_pos, '[');
            insert_pos++;
            gap_insert(gb, insert_pos, '^');
            insert_pos++;
            for (const char *p = notes[i].id; *p; p++) {
                gap_insert(gb, insert_pos, *p);
                insert_pos++;
            }
            gap_insert(gb, insert_pos, ']');
            insert_pos++;
            gap_insert(gb, insert_pos, ':');
            insert_pos++;
            gap_insert(gb, insert_pos, ' ');
            insert_pos++;
            gap_insert(gb, insert_pos, '\n');
            insert_pos++;
            gap_insert(gb, insert_pos, '\n');
            insert_pos++;
        }
    }

    free(notes);
    return first_new;
}

// #endregion

// #region Footnote Navigation

void footnote_jump(GapBuffer *gb, size_t *cursor) {
    size_t len = gap_len(gb);
    size_t cur = *cursor;

    // Check if cursor is in a footnote reference
    size_t id_start, id_len, total;
    if (md_check_footnote_ref(gb, cur, &id_start, &id_len, &total)) {
        // Extract ID
        char id[64];
        size_t idl = id_len < sizeof(id) - 1 ? id_len : sizeof(id) - 1;
        for (size_t i = 0; i < idl; i++) {
            id[i] = gap_at(gb, id_start + i);
        }
        id[idl] = '\0';

        // Find definition
        for (size_t pos = 0; pos < len; pos++) {
            size_t def_id_start, def_id_len, content_start, def_total;
            if (md_check_footnote_def(gb, pos, &def_id_start, &def_id_len, &content_start, &def_total)) {
                char def_id[64];
                size_t dl = def_id_len < sizeof(def_id) - 1 ? def_id_len : sizeof(def_id) - 1;
                for (size_t i = 0; i < dl; i++) {
                    def_id[i] = gap_at(gb, def_id_start + i);
                }
                def_id[dl] = '\0';

                if (strcmp(id, def_id) == 0) {
                    *cursor = content_start;
                    return;
                }
            }
        }

        // No definition found - create it
        size_t new_pos = create_missing_footnotes(gb);
        if (new_pos != SIZE_MAX) {
            // Find our specific definition
            for (size_t pos = new_pos; pos < gap_len(gb); pos++) {
                size_t def_id_start, def_id_len, content_start, def_total;
                if (md_check_footnote_def(gb, pos, &def_id_start, &def_id_len, &content_start, &def_total)) {
                    char def_id[64];
                    size_t dl = def_id_len < sizeof(def_id) - 1 ? def_id_len : sizeof(def_id) - 1;
                    for (size_t i = 0; i < dl; i++) {
                        def_id[i] = gap_at(gb, def_id_start + i);
                    }
                    def_id[dl] = '\0';

                    if (strcmp(id, def_id) == 0) {
                        *cursor = content_start;
                        return;
                    }
                }
            }
        }
        return;
    }

    // Check if we're somewhere that could be start of a reference (scan back to find it)
    for (size_t back = 0; back < 10 && cur >= back; back++) {
        size_t check_pos = cur - back;
        if (md_check_footnote_ref(gb, check_pos, &id_start, &id_len, &total)) {
            if (check_pos + total > cur) {
                // We're inside this reference - recurse with cursor at start
                size_t saved = cur;
                *cursor = check_pos;
                footnote_jump(gb, cursor);
                if (*cursor == check_pos) {
                    *cursor = saved;  // Restore if jump failed
                }
                return;
            }
        }
    }

    // Check if cursor is in a footnote definition
    size_t line_start = cur;
    while (line_start > 0 && gap_at(gb, line_start - 1) != '\n') {
        line_start--;
    }

    size_t def_id_start, def_id_len, content_start, def_total;
    if (md_check_footnote_def(gb, line_start, &def_id_start, &def_id_len, &content_start, &def_total)) {
        // Extract ID
        char id[64];
        size_t idl = def_id_len < sizeof(id) - 1 ? def_id_len : sizeof(id) - 1;
        for (size_t i = 0; i < idl; i++) {
            id[i] = gap_at(gb, def_id_start + i);
        }
        id[idl] = '\0';

        // Find first reference
        for (size_t pos = 0; pos < len; pos++) {
            size_t ref_id_start, ref_id_len, ref_total;
            if (md_check_footnote_ref(gb, pos, &ref_id_start, &ref_id_len, &ref_total)) {
                char ref_id[64];
                size_t rl = ref_id_len < sizeof(ref_id) - 1 ? ref_id_len : sizeof(ref_id) - 1;
                for (size_t i = 0; i < rl; i++) {
                    ref_id[i] = gap_at(gb, ref_id_start + i);
                }
                ref_id[rl] = '\0';

                if (strcmp(id, ref_id) == 0) {
                    *cursor = pos;
                    return;
                }
            }
        }
    }
}

bool footnote_create_definition(GapBuffer *gb, const char *id) {
    size_t len = gap_len(gb);

    // Check if definition already exists
    for (size_t pos = 0; pos < len; pos++) {
        size_t def_id_start, def_id_len, content_start, def_total;
        if (md_check_footnote_def(gb, pos, &def_id_start, &def_id_len, &content_start, &def_total)) {
            char def_id[64];
            size_t dl = def_id_len < sizeof(def_id) - 1 ? def_id_len : sizeof(def_id) - 1;
            for (size_t i = 0; i < dl; i++) {
                def_id[i] = gap_at(gb, def_id_start + i);
            }
            def_id[dl] = '\0';
            if (strcmp(id, def_id) == 0) {
                return false;  // Already exists
            }
        }
    }

    // Check if this is the first footnote definition
    bool first_footnote = true;
    for (size_t pos = 0; pos < len; pos++) {
        size_t d_id_start, d_id_len, d_content, d_total;
        if (md_check_footnote_def(gb, pos, &d_id_start, &d_id_len, &d_content, &d_total)) {
            first_footnote = false;
            break;
        }
    }

    size_t insert_pos = len;

    // Ensure blank line before footnotes section
    if (len > 0 && gap_at(gb, len - 1) != '\n') {
        gap_insert(gb, insert_pos, '\n');
        insert_pos++;
    }
    gap_insert(gb, insert_pos, '\n');
    insert_pos++;

    // Add separator if this is the first footnote
    if (first_footnote) {
        const char *sep = "---\n\n";
        for (const char *p = sep; *p; p++) {
            gap_insert(gb, insert_pos, *p);
            insert_pos++;
        }
    }

    // Insert [^id]:
    gap_insert(gb, insert_pos, '[');
    insert_pos++;
    gap_insert(gb, insert_pos, '^');
    insert_pos++;
    for (const char *p = id; *p; p++) {
        gap_insert(gb, insert_pos, *p);
        insert_pos++;
    }
    gap_insert(gb, insert_pos, ']');
    insert_pos++;
    gap_insert(gb, insert_pos, ':');
    insert_pos++;
    gap_insert(gb, insert_pos, ' ');

    return true;
}

// #endregion
