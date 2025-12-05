//! @file lang/bash.c
//! @brief Bash/Shell language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "#.*(?:\\n|$)", .token = HL_TOKEN_CMNT },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "\\s-[a-zA-Z]+|\\b(?:unset|readonly|shift|export|if|fi|else|elif|while|do|done|for|until|case|esac|break|continue|exit|return|trap|wait|eval|exec|then|declare|enable|local|select|typeset|time|add|remove|install|update|delete)(?=\\s|$)", .token = HL_TOKEN_KWD },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "(?<=\\s|^)(?:true|false)(?=\\s|$)", .token = HL_TOKEN_BOOL },
    { .pattern = "[=(){}<>!]+|[&|;]+", .token = HL_TOKEN_OPER },
    { .pattern = "\\$\\w+|\\$\\{[^}]*\\}|\\$\\([^)]*\\)", .token = HL_TOKEN_VAR },
    { .pattern = "(?<=^|\\||&&|;\\s*)[a-z_.-]+(?=\\s|$)", .token = HL_TOKEN_FUNC, .flags = HL_RULE_MULTILINE },
};

static const hl_detect_rule_t detect[] = {
    { "#!(/usr)?/bin/bash", 500 },
    { "\\b(if|elif|then|fi|echo)\\b|\\$", 10 },
};

static const hl_lang_def_t lang = {
    .name = "bash",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_bash(void) { return &lang; }
