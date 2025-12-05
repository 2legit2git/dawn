//! @file lang/py.c
//! @brief Python language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "#.*(?:\\n|$)", .token = HL_TOKEN_CMNT },
    { .pattern = "(\"\"\"|''')(?:\\\\[\\s\\S]|(?!\\1)[\\s\\S])*\\1?", .token = HL_TOKEN_CMNT },
    { .pattern = "f([\"'])(?:\\\\[\\s\\S]|(?!\\1).)*\\1?", .token = HL_TOKEN_STR, .flags = HL_RULE_CASELESS },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "\\b(?:and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "\\b(?:False|True|None)\\b", .token = HL_TOKEN_BOOL },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "[a-z_]\\w*(?=\\s*\\()", .token = HL_TOKEN_FUNC, .flags = HL_RULE_CASELESS },
    { .pattern = "[-/*+<>,=!&|^%]+", .token = HL_TOKEN_OPER },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "@[a-zA-Z_][\\w.]*", .token = HL_TOKEN_FUNC },
};

static const hl_detect_rule_t detect[] = {
    { "#!(/usr)?/bin/(python|python3)", 500 },
    { "\\b(def|print|class|and|or|lambda)\\b", 10 },
};

static const hl_lang_def_t lang = {
    .name = "py",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_py(void) { return &lang; }
