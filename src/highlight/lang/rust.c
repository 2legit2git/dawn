//! @file lang/rust.c
//! @brief Rust language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "//.*(?:\\n|$)|/\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "r#*\"[\\s\\S]*?\"#*", .token = HL_TOKEN_STR },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "\\b(?:as|break|const|continue|crate|else|enum|extern|false|fn|for|if|impl|in|let|loop|match|mod|move|mut|pub|ref|return|self|Self|static|struct|super|trait|true|type|unsafe|use|where|while|async|await|dyn)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "[/*+:?&|%^~=!,<>.^-]+", .token = HL_TOKEN_OPER },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "[a-zA-Z_][\\w_]*(?=\\s*!?\\s*\\()", .token = HL_TOKEN_FUNC },
    { .pattern = "'[a-zA-Z_][\\w_]*", .token = HL_TOKEN_TYPE },
    { .pattern = "#!?\\[[^\\]]*\\]", .token = HL_TOKEN_FUNC },
};

static const hl_detect_rule_t detect[] = {
    { "^\\s*(use|fn|mut|match)\\b", 100 },
};

static const hl_lang_def_t lang = {
    .name = "rust",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_rust(void) { return &lang; }
