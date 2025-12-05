//! @file lang/ts.c
//! @brief TypeScript language definition

#include "../highlight.h"

static const hl_lang_rule_t rules[] = {
    { .pattern = ":\\s*(?:any|void|number|boolean|string|object|never|enum)\\b", .token = HL_TOKEN_TYPE },
    { .pattern = "\\b(?:type|namespace|typedef|interface|public|private|protected|implements|declare|abstract|readonly)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "/\\*\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "//.*(?:\\n|$)|/\\*(?:(?!\\*/).|[\\s\\S])*(?:\\*/)?", .token = HL_TOKEN_CMNT },
    { .pattern = "`(?:[^`\\\\]|\\\\[\\s\\S])*`?", .token = HL_TOKEN_STR },
    { .pattern = "([\"'])(?:\\\\[\\s\\S]|(?!\\1)[^\\r\\n\\\\])*\\1?", .token = HL_TOKEN_STR },
    { .pattern = "=>|\\b(?:this|set|get|as|async|await|break|case|catch|class|const|constructor|continue|debugger|default|delete|do|else|enum|export|extends|finally|for|from|function|if|implements|import|in|instanceof|interface|let|var|of|new|package|private|protected|public|return|static|super|switch|throw|throws|try|typeof|void|while|with|yield)\\b", .token = HL_TOKEN_KWD },
    { .pattern = "(?:\\.e?|\\b)\\d(?:e-|[\\d.oxa-fA-F_])*(?:\\.|\\b)", .token = HL_TOKEN_NUM },
    { .pattern = "\\b(?:NaN|null|undefined|Infinity)\\b", .token = HL_TOKEN_NUM },
    { .pattern = "\\b(?:true|false)\\b", .token = HL_TOKEN_BOOL },
    { .pattern = "[/*+:?&|%^~=!,<>.^-]+", .token = HL_TOKEN_OPER },
    { .pattern = "\\b[A-Z][\\w_]*\\b", .token = HL_TOKEN_CLASS },
    { .pattern = "[a-zA-Z$_][\\w$_]*(?=\\s*(?:(?:\\?\\.)?\\s*\\(|=\\s*(?:\\(?[\\w,{}\\[\\])]+\\)?\\s*=>|function\\b)))", .token = HL_TOKEN_FUNC },
};

static const hl_detect_rule_t detect[] = {
    { "\\b(console|await|async|function|export|import|this|class|for|let|const|map|join|require|implements|interface|namespace)\\b", 10 },
};

static const hl_lang_def_t lang = {
    .name = "ts",
    .rules = rules,
    .rule_count = sizeof(rules) / sizeof(rules[0]),
    .default_token = HL_TOKEN_NONE,
    .detect = detect,
    .detect_count = sizeof(detect) / sizeof(detect[0]),
};

const hl_lang_def_t *hl_lang_ts(void) { return &lang; }
