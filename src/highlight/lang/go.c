//! @file lang/go.c
//! @brief Go language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "//.*(?:\\n|$)|/\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "`[^`]*`", .token = HL_TOKEN_STR },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "\\b(?:break|case|chan|const|continue|default|defer|else|fallthrough|for|func|go|goto|if|import|interface|map|package|range|return|select|struct|switch|type|var)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "\\b(?:true|false|nil)\\b", .token = HL_TOKEN_BOOL },
    { .pattern = "[a-zA-Z_][\\w_]*(?=\\s*\\()", .token = HL_TOKEN_FUNC },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "[+\\-*/%&|^~=!<>.^-]+", .token = HL_TOKEN_OPER },
};

static const hl_detect_rule_t detect[] = {
    { "\\b(func|fmt|package)\\b", 100 },
};

static const hl_lang_def_t lang = {
    .name = "go",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_go(void) { return &lang; }
