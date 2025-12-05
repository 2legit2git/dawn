//! @file lang/java.c
//! @brief Java language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "//.*(?:\\n|$)|/\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "\\b(?:abstract|assert|boolean|break|byte|case|catch|char|class|continue|const|default|do|double|else|enum|exports|extends|final|finally|float|for|goto|if|implements|import|instanceof|int|interface|long|module|native|new|package|private|protected|public|requires|return|short|static|strictfp|super|switch|synchronized|this|throw|throws|transient|try|var|void|volatile|while)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "[/*+:?&|%^~=!,<>.^-]+", .token = HL_TOKEN_OPER },
    { .pattern = "[a-zA-Z_][\\w_]*(?=\\s*\\()", .token = HL_TOKEN_FUNC },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "\\b(?:true|false|null)\\b", .token = HL_TOKEN_BOOL },
};

static const hl_detect_rule_t detect[] = {
    { "^import\\s+java", 500 },
};

static const hl_lang_def_t lang = {
    .name = "java",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_java(void) { return &lang; }
