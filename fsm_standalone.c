#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* Token types */
#define TS_FSM_SPECIFIC  0
#define TS_FSM_WILDCARD  1
#define TS_FSM_CNTRL     2
#define TS_FSM_LOWER     3
#define TS_FSM_UPPER     4
#define TS_FSM_PUNCT     5
#define TS_FSM_SPACE     6
#define TS_FSM_DIGIT     7
#define TS_FSM_XDIGIT    8
#define TS_FSM_ALPHA     9
#define TS_FSM_ALNUM     10
#define TS_FSM_PRINT     11
#define TS_FSM_GRAPH     12
#define TS_FSM_ASCII     13
#define TS_FSM_TYPE_MAX  13

/* Token recurrence types */
#define TS_FSM_SINGLE       0  /* Single occurrence */
#define TS_FSM_PERHAPS      1  /* Zero or one occurrence */
#define TS_FSM_MULTI        2  /* One or more occurrences */
#define TS_FSM_ANY          3  /* Zero or more occurrences */
#define TS_FSM_HEAD_IGNORE  4  /* Ignore pattern at head */

/* Character type flags */
#define _C  0x01  /* control */
#define _U  0x02  /* upper */
#define _L  0x04  /* lower */
#define _D  0x08  /* digit */
#define _P  0x10  /* punct */
#define _S  0x20  /* space */
#define _X  0x40  /* hex digit */
#define _SP 0x80  /* space */
#define _A  0x100 /* ascii */
#define _W  0x200 /* wildcard */

struct fsm_token {
    uint8_t  type;      /* Token type (specific char or character class) */
    uint8_t  recur;     /* Recurrence type */
    uint8_t  value;     /* Specific character value if type is TS_FSM_SPECIFIC */
};

struct fsm_pattern {
    unsigned int ntokens;
    struct fsm_token *tokens;
};

/* Map to character type flags */
static const uint16_t token_map[TS_FSM_TYPE_MAX + 1] = {
    [TS_FSM_SPECIFIC] = 0,
    [TS_FSM_WILDCARD] = _W,
    [TS_FSM_CNTRL]    = _C,
    [TS_FSM_LOWER]    = _L,
    [TS_FSM_UPPER]    = _U,
    [TS_FSM_PUNCT]    = _P,
    [TS_FSM_SPACE]    = _S,
    [TS_FSM_DIGIT]    = _D,
    [TS_FSM_XDIGIT]   = _D | _X,
    [TS_FSM_ALPHA]    = _U | _L,
    [TS_FSM_ALNUM]    = _U | _L | _D,
    [TS_FSM_PRINT]    = _P | _U | _L | _D | _SP,
    [TS_FSM_GRAPH]    = _P | _U | _L | _D,
    [TS_FSM_ASCII]    = _A,
};

/* Character lookup table */
static const uint16_t char_lookup_tbl[256] = {
    /* ASCII control characters (0-31) */
    [0 ... 31] = _W|_A|_C,
    
    /* ASCII printable characters (32-127) */
    [' '] = _W|_A|_S|_SP,
    ['!'] = _W|_A|_P,
    ['"'] = _W|_A|_P,
    ['#'] = _W|_A|_P,
    ['$'] = _W|_A|_P,
    ['%'] = _W|_A|_P,
    ['&'] = _W|_A|_P,
    ['\''] = _W|_A|_P,
    ['('] = _W|_A|_P,
    [')'] = _W|_A|_P,
    ['*'] = _W|_A|_P,
    ['+'] = _W|_A|_P,
    [','] = _W|_A|_P,
    ['-'] = _W|_A|_P,
    ['.'] = _W|_A|_P,
    ['/'] = _W|_A|_P,
    
    /* Digits */
    ['0' ... '9'] = _W|_A|_D,
    
    /* More punctuation */
    [':'] = _W|_A|_P,
    [';'] = _W|_A|_P,
    ['<'] = _W|_A|_P,
    ['='] = _W|_A|_P,
    ['>'] = _W|_A|_P,
    ['?'] = _W|_A|_P,
    ['@'] = _W|_A|_P,
    
    /* Uppercase letters */
    ['A' ... 'F'] = _W|_A|_U|_X,
    ['G' ... 'Z'] = _W|_A|_U,
    
    /* More punctuation */
    ['['] = _W|_A|_P,
    ['\\'] = _W|_A|_P,
    [']'] = _W|_A|_P,
    ['^'] = _W|_A|_P,
    ['_'] = _W|_A|_P,
    ['`'] = _W|_A|_P,
    
    /* Lowercase letters */
    ['a' ... 'f'] = _W|_A|_L|_X,
    ['g' ... 'z'] = _W|_A|_L,
    
    /* More punctuation */
    ['{'] = _W|_A|_P,
    ['|'] = _W|_A|_P,
    ['}'] = _W|_A|_P,
    ['~'] = _W|_A|_P,
    
    /* DEL */
    [127] = _W|_A|_C,
    
    /* Extended ASCII */
    [128 ... 255] = _W
};

static inline int match_token(struct fsm_token *t, uint8_t c) {
    if (t->type)
        return (char_lookup_tbl[c] & token_map[t->type]) != 0;
    else
        return t->value == c;
}

/* Find pattern in text, returns position if found or -1 if not found */
int fsm_find(const char *text, size_t text_len, struct fsm_pattern *pattern) {
    struct fsm_token *cur = NULL, *next;
    unsigned int match_start = 0;
    unsigned int pos = 0;
    int strict = pattern->tokens[0].recur != TS_FSM_HEAD_IGNORE;

startover:
    match_start = pos;

    for (unsigned int tok_idx = 0; tok_idx < pattern->ntokens; tok_idx++) {
        cur = &pattern->tokens[tok_idx];
        next = (tok_idx < pattern->ntokens - 1) ? &pattern->tokens[tok_idx + 1] : NULL;

        switch (cur->recur) {
        case TS_FSM_SINGLE:
            if (pos >= text_len)
                goto no_match;
            if (!match_token(cur, text[pos])) {
                if (strict)
                    goto no_match;
                pos++;
                goto startover;
            }
            break;

        case TS_FSM_PERHAPS:
            if (pos >= text_len || !match_token(cur, text[pos]))
                continue;
            break;

        case TS_FSM_MULTI:
            if (pos >= text_len)
                goto no_match;
            if (!match_token(cur, text[pos])) {
                if (strict)
                    goto no_match;
                pos++;
                goto startover;
            }
            pos++;
            /* fallthrough */

        case TS_FSM_ANY:
            if (next == NULL)
                goto found_match;
            if (pos >= text_len)
                continue;
            while (!match_token(next, text[pos])) {
                if (!match_token(cur, text[pos])) {
                    if (strict)
                        goto no_match;
                    pos++;
                    goto startover;
                }
                pos++;
                if (pos >= text_len)
                    goto no_match;
            }
            continue;

        case TS_FSM_HEAD_IGNORE:
            if (pos >= text_len)
                continue;
            while (!match_token(next, text[pos])) {
                if (!match_token(cur, text[pos]))
                    goto no_match;
                pos++;
                if (pos >= text_len)
                    goto no_match;
            }
            match_start = pos;
            continue;
        }

        pos++;
    }

    if (pos <= text_len)
        goto found_match;

no_match:
    return -1;

found_match:
    return match_start;
}

/* Helper function to create a simple pattern for exact string matching */
struct fsm_pattern *create_exact_pattern(const char *str) {
    size_t len = strlen(str);
    struct fsm_pattern *pattern = malloc(sizeof(struct fsm_pattern));
    if (!pattern)
        return NULL;

    pattern->tokens = malloc(len * sizeof(struct fsm_token));
    if (!pattern->tokens) {
        free(pattern);
        return NULL;
    }

    pattern->ntokens = len;
    for (size_t i = 0; i < len; i++) {
        pattern->tokens[i].type = TS_FSM_SPECIFIC;
        pattern->tokens[i].recur = TS_FSM_SINGLE;
        pattern->tokens[i].value = str[i];
    }

    return pattern;
}

/* Helper function to create a pattern that matches digits */
struct fsm_pattern *create_digit_pattern(int min_digits, int max_digits) {
    struct fsm_pattern *pattern = malloc(sizeof(struct fsm_pattern));
    if (!pattern)
        return NULL;

    pattern->tokens = malloc(sizeof(struct fsm_token));
    if (!pattern->tokens) {
        free(pattern);
        return NULL;
    }

    pattern->ntokens = 1;
    pattern->tokens[0].type = TS_FSM_DIGIT;
    pattern->tokens[0].recur = (min_digits == 0) ? 
        ((max_digits == 1) ? TS_FSM_PERHAPS : TS_FSM_ANY) :
        ((max_digits == 1) ? TS_FSM_SINGLE : TS_FSM_MULTI);
    pattern->tokens[0].value = 0;

    return pattern;
}

void free_pattern(struct fsm_pattern *pattern) {
    if (pattern) {
        free(pattern->tokens);
        free(pattern);
    }
}

int main(void) {
    // Test 1: Exact string matching
    printf("Test 1: Exact string matching\n");
    const char *text1 = "Hello, World! This is a test string.";
    struct fsm_pattern *pattern1 = create_exact_pattern("World");
    
    int pos1 = fsm_find(text1, strlen(text1), pattern1);
    printf("Text: %s\n", text1);
    printf("Pattern: 'World'\n");
    printf("Result: %s (position: %d)\n\n", 
           pos1 >= 0 ? "Found" : "Not found", pos1);

    // Test 2: Digit sequence matching
    printf("Test 2: Digit sequence matching\n");
    const char *text2 = "The year is 2024 and the price is $99.99";
    struct fsm_pattern *pattern2 = create_digit_pattern(1, 4);
    
    int pos2 = fsm_find(text2, strlen(text2), pattern2);
    printf("Text: %s\n", text2);
    printf("Pattern: [digit sequence]\n");
    printf("Result: %s (position: %d)\n\n",
           pos2 >= 0 ? "Found" : "Not found", pos2);

    // Test 3: Pattern not found
    printf("Test 3: Pattern not found\n");
    const char *text3 = "Simple text without numbers";
    struct fsm_pattern *pattern3 = create_digit_pattern(1, 1);
    
    int pos3 = fsm_find(text3, strlen(text3), pattern3);
    printf("Text: %s\n", text3);
    printf("Pattern: [single digit]\n");
    printf("Result: %s (position: %d)\n",
           pos3 >= 0 ? "Found" : "Not found", pos3);

    // Cleanup
    free_pattern(pattern1);
    free_pattern(pattern2);
    free_pattern(pattern3);

    return 0;
}
