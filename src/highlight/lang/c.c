//! @file lang/c.c
//! @brief C language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = "//.*(?:\\n|$)|/\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "'(?:\\\\[\\s\\S]|[^'\\\\])'", .token = HL_TOKEN_STR },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "#\\s*include\\s*(?:<[^>]*>|\"[^\"]*\")", .token = HL_TOKEN_KWD },
    { .pattern = "#\\s*(?:define|undef|ifdef|ifndef|if|elif|else|endif|error|pragma|warning|line)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "\\b(?:auto|break|case|char|const|continue|default|do|double|else|enum|extern|float|for|goto|if|inline|int|long|register|restrict|return|short|signed|sizeof|static|struct|switch|typedef|union|unsigned|void|volatile|while|_Alignas|_Alignof|_Atomic|_Bool|_Complex|_Generic|_Imaginary|_Noreturn|_Static_assert|_Thread_local)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "[*&]", .token = HL_TOKEN_OPER },
    { .pattern = "[/*+:?|%^~=!,<>.^-]+", .token = HL_TOKEN_OPER },
    { .pattern = "[a-zA-Z_][\\w_]*(?=\\s*\\()", .token = HL_TOKEN_FUNC },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "\\b(?:size_t|ssize_t|ptrdiff_t|intptr_t|uintptr_t|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t|bool|FILE|NULL)\\b", .token = HL_TOKEN_TYPE },
    { .pattern = "\\b(?:true|false)\\b", .token = HL_TOKEN_BOOL },
};

static const hl_detect_rule_t detect[] = {
    { "#include\\b|\\bprintf\\s*\\(", 100 },
};

static const hl_lang_def_t lang = {
    .name = "c",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_c(void) { return &lang; }
