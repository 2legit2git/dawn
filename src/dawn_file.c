// dawn_file.c

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "dawn_file.h"
#include "dawn_gap.h"
#include "dawn_chat.h"
#include "dawn_utils.h"
#include "dawn_image.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

// #region History Directory

char *history_dir(void) {
    static char path[PATH_MAX];
    const PlatformBackend *p = platform_get();
    const char *home = p && p->get_home_dir ? p->get_home_dir() : "/tmp";
    snprintf(path, sizeof(path), "%s/%s", home ? home : "/tmp", HISTORY_DIR_NAME);
    return path;
}

// #endregion

// #region User Info

//! Get current user's display name for document metadata
static const char *get_username(void) {
    const PlatformBackend *p = platform_get();
    if (p && p->get_username) {
        return p->get_username();
    }
    return "Unknown";
}

// #endregion

// #region Session Persistence

void save_session(void) {
    const PlatformBackend *p = platform_get();
    if (!p) return;

    if (gap_len(&app.text) == 0) return;

    // Ensure history directory exists
    p->mkdir_p(history_dir());

    // Generate new path if first save
    if (!app.session_path) {
        static char path[PATH_MAX];
        PlatformLocalTime lt;
        p->get_local_time(&lt);
        snprintf(path, sizeof(path), "%s/%04d-%02d-%02d_%02d%02d%02d.md",
                 history_dir(), lt.year, lt.mon + 1, lt.mday, lt.hour, lt.min, lt.sec);
        app.session_path = strdup(path);
    }

    // Build content with YAML frontmatter
    char *txt = gap_to_str(&app.text);
    size_t txt_len = strlen(txt);

    PlatformLocalTime lt;
    p->get_local_time(&lt);

    // Estimate frontmatter size
    size_t fm_size = 256;
    char *content = malloc(fm_size + txt_len + 16);

    int fm_len = snprintf(content, fm_size,
        "---\n"
        "title: %s\n"
        "author: %s\n"
        "date: %04d-%02d-%02d\n"
        "---\n\n",
        app.session_title ? app.session_title : "Untitled",
        get_username(),
        lt.year, lt.mon + 1, lt.mday);

    memcpy(content + fm_len, txt, txt_len);
    content[fm_len + txt_len] = '\0';

    p->write_file(app.session_path, content, fm_len + txt_len);

    free(content);
    free(txt);

    // Save AI chat to companion .chat.json file
    if (app.chat_count > 0) {
        char chat_path[520];
        get_chat_path(app.session_path, chat_path, sizeof(chat_path));

        // Build JSON content
        size_t json_cap = 1024;
        char *json = malloc(json_cap);
        size_t json_len = 0;

        json_len += snprintf(json + json_len, json_cap - json_len, "[\n");

        for (int i = 0; i < app.chat_count; i++) {
            ChatMessage *m = &app.chat_msgs[i];

            // Ensure capacity
            while (json_len + m->len * 2 + 100 >= json_cap) {
                json_cap *= 2;
                json = realloc(json, json_cap);
            }

            json_len += snprintf(json + json_len, json_cap - json_len,
                "  {\"role\": \"%s\", \"content\": \"",
                m->is_user ? "user" : "assistant");

            // Escape JSON special characters
            for (size_t j = 0; j < m->len; j++) {
                char c = m->text[j];
                if (json_len + 10 >= json_cap) {
                    json_cap *= 2;
                    json = realloc(json, json_cap);
                }
                if (c == '"') { json[json_len++] = '\\'; json[json_len++] = '"'; }
                else if (c == '\\') { json[json_len++] = '\\'; json[json_len++] = '\\'; }
                else if (c == '\n') { json[json_len++] = '\\'; json[json_len++] = 'n'; }
                else if (c == '\r') { json[json_len++] = '\\'; json[json_len++] = 'r'; }
                else if (c == '\t') { json[json_len++] = '\\'; json[json_len++] = 't'; }
                else if ((uint8_t)c >= 32) json[json_len++] = c;
            }

            json_len += snprintf(json + json_len, json_cap - json_len,
                "\"}%s\n", i < app.chat_count - 1 ? "," : "");
        }

        json_len += snprintf(json + json_len, json_cap - json_len, "]\n");

        p->write_file(chat_path, json, json_len);
        free(json);
    }
}

//! Parse frontmatter title from a session file
//! @param path path to .md file
//! @return newly allocated title string, or NULL if not found
static char *parse_frontmatter_title(const char *path) {
    const PlatformBackend *p = platform_get();
    if (!p || !p->read_file) return NULL;

    size_t len;
    char *content = p->read_file(path, &len);
    if (!content) return NULL;

    char *title = NULL;

    // Check for opening ---
    if (len >= 4 && strncmp(content, "---\n", 4) == 0) {
        char *end = strstr(content + 4, "\n---");
        if (end) {
            char *title_line = strstr(content, "title:");
            if (title_line && title_line < end) {
                char *val = title_line + 6;
                while (*val == ' ') val++;
                char *nl = strchr(val, '\n');
                if (nl && nl < end) {
                    size_t title_len = nl - val;
                    title = malloc(title_len + 1);
                    memcpy(title, val, title_len);
                    title[title_len] = '\0';
                }
            }
        }
    }

    free(content);
    return title;
}

void load_history(void) {
    const PlatformBackend *p = platform_get();
    if (!p) return;

    // Free existing history
    if (app.history) {
        for (int i = 0; i < app.hist_count; i++) {
            free(app.history[i].path);
            free(app.history[i].title);
            free(app.history[i].date_str);
        }
        free(app.history);
        app.history = NULL;
        app.hist_count = 0;
    }

    char **names;
    int count;
    if (!p->list_dir(history_dir(), &names, &count)) {
        return;
    }

    int cap = 64;
    app.history = malloc(sizeof(HistoryEntry) * (size_t)cap);

    for (int i = 0; i < count; i++) {
        // Only .md files, skip .chat.json
        size_t nlen = strlen(names[i]);
        if (nlen < 3 || strcmp(names[i] + nlen - 3, ".md") != 0) {
            free(names[i]);
            continue;
        }

        if (app.hist_count >= cap) {
            cap *= 2;
            app.history = realloc(app.history, sizeof(HistoryEntry) * (size_t)cap);
        }

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", history_dir(), names[i]);

        HistoryEntry *entry = &app.history[app.hist_count];
        entry->path = strdup(full);
        entry->title = parse_frontmatter_title(full);

        // Parse date from filename (format: YYYY-MM-DD_HHMM.md)
        int y, mo, da, h, mi;
        if (sscanf(names[i], "%d-%d-%d_%d%d", &y, &mo, &da, &h, &mi) == 5) {
            static const char *months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                           "Jul","Aug","Sep","Oct","Nov","Dec"};
            char date_buf[64];
            snprintf(date_buf, sizeof(date_buf), "%s %d, %d at %d:%02d",
                     months[mo], da, y, h, mi);
            entry->date_str = strdup(date_buf);
        } else {
            entry->date_str = strdup(names[i]);
        }

        app.hist_count++;
        free(names[i]);
    }
    free(names);

    // Sort newest first (by path, which contains date)
    for (int i = 0; i < app.hist_count - 1; i++) {
        for (int j = i + 1; j < app.hist_count; j++) {
            if (strcmp(app.history[i].path, app.history[j].path) < 0) {
                HistoryEntry tmp = app.history[i];
                app.history[i] = app.history[j];
                app.history[j] = tmp;
            }
        }
    }
}

void load_chat_history(const char *session_path) {
    const PlatformBackend *p = platform_get();
    if (!p || !p->read_file) return;

    char chat_path[520];
    get_chat_path(session_path, chat_path, sizeof(chat_path));

    size_t size;
    char *json_str = p->read_file(chat_path, &size);
    if (!json_str) return;

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return;

    if (cJSON_IsArray(root)) {
        cJSON *msg;
        cJSON_ArrayForEach(msg, root) {
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(role) && cJSON_IsString(content)) {
                bool is_user = strcmp(role->valuestring, "user") == 0;
                chat_add(content->valuestring, is_user);
            }
        }
    }

    cJSON_Delete(root);
}

// #endregion

// #region File Operations

void load_file_for_editing(const char *path) {
    const PlatformBackend *p = platform_get();
    if (!p || !p->read_file) return;

    size_t size;
    char *content = p->read_file(path, &size);
    if (!content) return;

    // Parse and strip frontmatter
    free(app.session_title);
    app.session_title = NULL;

    char *text_start = content;
    if (size >= 4 && strncmp(content, "---\n", 4) == 0) {
        char *end = strstr(content + 4, "\n---");
        if (end) {
            // Extract title from frontmatter
            char *title_line = strstr(content, "title:");
            if (title_line && title_line < end) {
                char *val = title_line + 6;
                while (*val == ' ') val++;
                char *nl = strchr(val, '\n');
                if (nl && nl < end) {
                    size_t len = nl - val;
                    app.session_title = malloc(len + 1);
                    memcpy(app.session_title, val, len);
                    app.session_title[len] = '\0';
                }
            }
            // Skip past frontmatter
            text_start = end + 4;  // Skip \n---
            if (*text_start == '\n') text_start++;
            if (*text_start == '\n') text_start++;
        }
    }

    // Initialize gap buffer with content (normalize CRLF -> LF)
    gap_free(&app.text);
    gap_init(&app.text, 4096);
    size_t text_len = strlen(text_start);
    if (text_len > 0) {
        text_len = normalize_line_endings(text_start, text_len);
        gap_insert_str(&app.text, 0, text_start, text_len);
    }
    free(content);

    // Clear image cache when switching documents
    image_clear_all();

    // Get base directory from file path for image resolution
    char base_dir[512] = "";
    strncpy(base_dir, path, sizeof(base_dir) - 1);
    char *last_slash = strrchr(base_dir, '/');
    if (last_slash) *last_slash = '\0';
    else base_dir[0] = '\0';

    // Reset editor state
    free(app.session_path);
    app.session_path = strdup(path);
    app.cursor = 0;
    app.scroll_y = 0;
    app.selecting = false;
    app.timer_done = false;
    app.timer_on = false;
    app.mode = MODE_WRITING;
    app.ai_open = false;
    app.ai_focused = false;
    app.ai_input_len = 0;
    app.ai_input_cursor = 0;
    app.chat_scroll = 0;
    chat_clear();

    // Load associated chat history
    load_chat_history(path);

    #if HAS_LIBAI
    if (app.ai_ready && !app.ai_session) {
        ai_init_session();
    }
    #endif

    if (p && p->set_title) {
        p->set_title(app.session_title);
    }
}

void open_in_finder(const char *path) {
    const PlatformBackend *p = platform_get();
    if (p && p->reveal_in_finder) {
        p->reveal_in_finder(path);
    }
}

// #endregion
