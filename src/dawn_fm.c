// dawn_fm.c - YAML frontmatter parsing and serialization

#include "dawn_fm.h"
#include <libfyaml.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// #region Types

struct Frontmatter {
    struct fy_document *doc;
    char *backing_buf;  // Original YAML string (libfyaml keeps references)
};

// #endregion

// #region Lifecycle

Frontmatter *fm_parse(const char *content, size_t len, size_t *consumed) {
    if (!content || len < 4) return NULL;
    if (consumed) *consumed = 0;

    // Check for opening delimiter
    if (strncmp(content, "---", 3) != 0) return NULL;
    if (content[3] != '\n' && content[3] != '\r') return NULL;

    // Find closing delimiter
    const char *start = content + 4;
    const char *end = NULL;

    // Search for \n--- or \r\n---
    for (const char *p = start; p < content + len - 3; p++) {
        if (*p == '\n' && strncmp(p + 1, "---", 3) == 0) {
            // Check that --- is followed by newline or EOF
            const char *after = p + 4;
            if (after >= content + len || *after == '\n' || *after == '\r') {
                end = p;
                break;
            }
        }
    }

    if (!end) return NULL;

    // Extract YAML content between delimiters
    size_t yaml_len = end - start;
    if (yaml_len == 0) {
        // Empty frontmatter
        if (consumed) {
            *consumed = (end + 4) - content;
            if (*consumed < len && content[*consumed] == '\n') (*consumed)++;
        }
        return fm_create();
    }

    // Parse YAML - copy the string since libfyaml keeps references to it
    char *yaml_copy = malloc(yaml_len + 1);
    if (!yaml_copy) return NULL;
    memcpy(yaml_copy, start, yaml_len);
    yaml_copy[yaml_len] = '\0';

    struct fy_document *doc = fy_document_build_from_string(NULL, yaml_copy, yaml_len);
    if (!doc) {
        free(yaml_copy);
        return NULL;
    }

    // Verify root is a mapping
    struct fy_node *root = fy_document_root(doc);
    if (!root || !fy_node_is_mapping(root)) {
        fy_document_destroy(doc);
        free(yaml_copy);
        return NULL;
    }

    Frontmatter *fm = malloc(sizeof(Frontmatter));
    if (!fm) {
        fy_document_destroy(doc);
        free(yaml_copy);
        return NULL;
    }
    fm->doc = doc;
    fm->backing_buf = yaml_copy;

    // Calculate consumed bytes
    if (consumed) {
        *consumed = (end + 4) - content;
        // Skip trailing newline after ---
        if (*consumed < len && content[*consumed] == '\n') (*consumed)++;
    }

    return fm;
}

Frontmatter *fm_create(void) {
    struct fy_document *doc = fy_document_create(NULL);
    if (!doc) return NULL;

    // Create empty block mapping as root (not flow style {})
    struct fy_node *root = fy_node_create_mapping(doc);
    if (!root) {
        fy_document_destroy(doc);
        return NULL;
    }
    fy_document_set_root(doc, root);

    Frontmatter *fm = malloc(sizeof(Frontmatter));
    if (!fm) {
        fy_document_destroy(doc);
        return NULL;
    }
    fm->doc = doc;
    fm->backing_buf = NULL;
    return fm;
}

void fm_free(Frontmatter *fm) {
    if (!fm) return;
    if (fm->doc) fy_document_destroy(fm->doc);
    free(fm->backing_buf);
    free(fm);
}

// #endregion

// #region Accessors

const char *fm_get_string(const Frontmatter *fm, const char *key) {
    if (!fm || !fm->doc || !key) return NULL;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root) return NULL;

    struct fy_node *node = fy_node_by_path(root, key, -1, FYNWF_DONT_FOLLOW);
    if (!node || !fy_node_is_scalar(node)) return NULL;

    return fy_node_get_scalar0(node);
}

int fm_get_int(const Frontmatter *fm, const char *key, int default_val) {
    const char *str = fm_get_string(fm, key);
    if (!str) return default_val;

    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0') return default_val;

    return (int)val;
}

bool fm_get_bool(const Frontmatter *fm, const char *key, bool default_val) {
    const char *str = fm_get_string(fm, key);
    if (!str) return default_val;

    if (strcmp(str, "true") == 0 || strcmp(str, "yes") == 0 ||
        strcmp(str, "on") == 0 || strcmp(str, "1") == 0) {
        return true;
    }
    if (strcmp(str, "false") == 0 || strcmp(str, "no") == 0 ||
        strcmp(str, "off") == 0 || strcmp(str, "0") == 0) {
        return false;
    }
    return default_val;
}

bool fm_has_key(const Frontmatter *fm, const char *key) {
    if (!fm || !fm->doc || !key) return false;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root) return false;

    struct fy_node *node = fy_node_by_path(root, key, -1, FYNWF_DONT_FOLLOW);
    return node != NULL;
}

//! Infer scalar type from content (YAML 1.1 rules)
static FmType infer_scalar_type(const char *str) {
    if (!str || *str == '\0') return FM_NULL;

    // Null patterns
    if (strcmp(str, "~") == 0 || strcmp(str, "null") == 0 ||
        strcmp(str, "Null") == 0 || strcmp(str, "NULL") == 0) {
        return FM_NULL;
    }

    // Boolean patterns
    if (strcmp(str, "true") == 0 || strcmp(str, "True") == 0 ||
        strcmp(str, "TRUE") == 0 || strcmp(str, "yes") == 0 ||
        strcmp(str, "Yes") == 0 || strcmp(str, "YES") == 0 ||
        strcmp(str, "on") == 0 || strcmp(str, "On") == 0 ||
        strcmp(str, "ON") == 0) {
        return FM_BOOL;
    }
    if (strcmp(str, "false") == 0 || strcmp(str, "False") == 0 ||
        strcmp(str, "FALSE") == 0 || strcmp(str, "no") == 0 ||
        strcmp(str, "No") == 0 || strcmp(str, "NO") == 0 ||
        strcmp(str, "off") == 0 || strcmp(str, "Off") == 0 ||
        strcmp(str, "OFF") == 0) {
        return FM_BOOL;
    }

    // Check for integer (optional sign, digits only)
    const char *p = str;
    if (*p == '+' || *p == '-') p++;
    if (*p == '\0') return FM_STRING;

    bool has_dot = false;
    bool has_exp = false;
    bool all_digits = true;

    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') continue;
        if (*p == '.' && !has_dot && !has_exp) {
            has_dot = true;
            continue;
        }
        if ((*p == 'e' || *p == 'E') && !has_exp) {
            has_exp = true;
            if (p[1] == '+' || p[1] == '-') p++;
            continue;
        }
        all_digits = false;
        break;
    }

    if (all_digits) {
        return (has_dot || has_exp) ? FM_FLOAT : FM_INT;
    }

    return FM_STRING;
}

FmType fm_get_type(const Frontmatter *fm, const char *key) {
    if (!fm || !fm->doc || !key) return FM_NULL;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root) return FM_NULL;

    struct fy_node *node = fy_node_by_path(root, key, -1, FYNWF_DONT_FOLLOW);
    if (!node) return FM_NULL;

    if (fy_node_is_mapping(node)) return FM_MAPPING;
    if (fy_node_is_sequence(node)) return FM_SEQUENCE;
    if (fy_node_is_null(node)) return FM_NULL;

    // It's a scalar - infer type from content
    const char *str = fy_node_get_scalar0(node);
    return infer_scalar_type(str);
}

const char *fm_type_name(FmType type) {
    switch (type) {
        case FM_NULL:     return "null";
        case FM_BOOL:     return "bool";
        case FM_INT:      return "int";
        case FM_FLOAT:    return "float";
        case FM_STRING:   return "string";
        case FM_SEQUENCE: return "list";
        case FM_MAPPING:  return "object";
    }
    return "unknown";
}

// #endregion

// #region Mutators

bool fm_set_string(Frontmatter *fm, const char *key, const char *value) {
    if (!fm || !fm->doc || !key) return false;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root || !fy_node_is_mapping(root)) return false;

    // First, remove existing key if present (collect pair to remove, then remove after iteration)
    struct fy_node_pair *to_remove = NULL;
    void *iter = NULL;
    struct fy_node_pair *pair;
    while ((pair = fy_node_mapping_iterate(root, &iter))) {
        struct fy_node *k = fy_node_pair_key(pair);
        if (k) {
            const char *kstr = fy_node_get_scalar0(k);
            if (kstr && strcmp(kstr, key) == 0) {
                to_remove = pair;
                break;
            }
        }
    }
    if (to_remove) {
        fy_node_mapping_remove(root, to_remove);
    }

    // Build key node
    struct fy_node *key_node = fy_node_create_scalar_copy(fm->doc, key, strlen(key));
    if (!key_node) return false;

    // Build value node
    struct fy_node *val_node;
    if (value) {
        val_node = fy_node_create_scalar_copy(fm->doc, value, strlen(value));
    } else {
        val_node = fy_node_build_from_string(fm->doc, "~", 1); // null
    }

    if (!val_node) {
        fy_node_free(key_node);
        return false;
    }

    // Append new key-value
    int ret = fy_node_mapping_append(root, key_node, val_node);
    return ret == 0;
}

bool fm_set_int(Frontmatter *fm, const char *key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return fm_set_string(fm, key, buf);
}

bool fm_set_bool(Frontmatter *fm, const char *key, bool value) {
    return fm_set_string(fm, key, value ? "true" : "false");
}

bool fm_remove(Frontmatter *fm, const char *key) {
    if (!fm || !fm->doc || !key) return false;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root || !fy_node_is_mapping(root)) return false;

    void *iter = NULL;
    struct fy_node_pair *pair;
    while ((pair = fy_node_mapping_iterate(root, &iter))) {
        struct fy_node *k = fy_node_pair_key(pair);
        if (k) {
            const char *kstr = fy_node_get_scalar0(k);
            if (kstr && strcmp(kstr, key) == 0) {
                fy_node_mapping_remove(root, pair);
                return true;
            }
        }
    }
    return false;
}

// #endregion

// #region Serialization

char *fm_to_string(const Frontmatter *fm, size_t *len) {
    if (!fm || !fm->doc) {
        if (len) *len = 0;
        return NULL;
    }

    // Check if empty
    if (fm_count(fm) == 0) {
        if (len) *len = 0;
        return NULL;
    }

    // Emit YAML content
    char *yaml = fy_emit_document_to_string(fm->doc, FYECF_DEFAULT | FYECF_NO_ENDING_NEWLINE);
    if (!yaml) {
        if (len) *len = 0;
        return NULL;
    }

    size_t yaml_len = strlen(yaml);

    // Build full frontmatter with delimiters: "---\n" + yaml + "\n---\n"
    size_t total = 4 + yaml_len + 5;
    char *result = malloc(total + 1);
    if (!result) {
        free(yaml);
        if (len) *len = 0;
        return NULL;
    }

    memcpy(result, "---\n", 4);
    memcpy(result + 4, yaml, yaml_len);
    memcpy(result + 4 + yaml_len, "\n---\n", 5);
    result[total] = '\0';

    free(yaml);
    if (len) *len = total;
    return result;
}

// #endregion

// #region Iteration

//! Get type for a node
static FmType get_node_type(struct fy_node *node) {
    if (!node || fy_node_is_null(node)) return FM_NULL;
    if (fy_node_is_mapping(node)) return FM_MAPPING;
    if (fy_node_is_sequence(node)) return FM_SEQUENCE;
    const char *str = fy_node_get_scalar0(node);
    return infer_scalar_type(str);
}

void fm_iterate(const Frontmatter *fm, FmIterCb cb, void *user_data) {
    if (!fm || !fm->doc || !cb) return;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root || !fy_node_is_mapping(root)) return;

    void *iter = NULL;
    struct fy_node_pair *pair;
    while ((pair = fy_node_mapping_iterate(root, &iter))) {
        struct fy_node *k = fy_node_pair_key(pair);
        struct fy_node *v = fy_node_pair_value(pair);

        const char *key_str = k ? fy_node_get_scalar0(k) : NULL;
        const char *val_str = NULL;
        FmType type = get_node_type(v);

        if (v && fy_node_is_scalar(v)) {
            val_str = fy_node_get_scalar0(v);
        }

        if (key_str) {
            FmEntry entry = { .key = key_str, .value = val_str, .type = type };
            if (!cb(&entry, user_data)) break;
        }
    }
}

int fm_count(const Frontmatter *fm) {
    if (!fm || !fm->doc) return 0;

    struct fy_node *root = fy_document_root(fm->doc);
    if (!root || !fy_node_is_mapping(root)) return 0;

    int count = 0;
    void *iter = NULL;
    while (fy_node_mapping_iterate(root, &iter)) {
        count++;
    }
    return count;
}

// #endregion
