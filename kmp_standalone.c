#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct kmp_pattern {
    unsigned char *pattern;
    unsigned int pattern_len;
    unsigned int *prefix_tbl;
};

// Compute the prefix table (also known as the "failure function" or "pi table")
static void compute_prefix_tbl(const unsigned char *pattern, unsigned int len,
                             unsigned int *prefix_tbl, int ignore_case)
{
    unsigned int k = 0, q;

    prefix_tbl[0] = 0;  // First entry is always 0
    
    for (q = 1; q < len; q++) {
        while (k > 0 && (ignore_case ? 
               toupper(pattern[k]) : pattern[k]) != 
               (ignore_case ? toupper(pattern[q]) : pattern[q])) {
            k = prefix_tbl[k-1];
        }
        
        if ((ignore_case ? toupper(pattern[k]) : pattern[k]) == 
            (ignore_case ? toupper(pattern[q]) : pattern[q])) {
            k++;
        }
        prefix_tbl[q] = k;
    }
}

// Initialize KMP pattern structure
struct kmp_pattern *kmp_init(const unsigned char *pattern, unsigned int len, int ignore_case)
{
    struct kmp_pattern *kmp;
    
    kmp = malloc(sizeof(*kmp));
    if (!kmp)
        return NULL;
        
    kmp->pattern = malloc(len);
    if (!kmp->pattern) {
        free(kmp);
        return NULL;
    }
    
    kmp->prefix_tbl = malloc(len * sizeof(unsigned int));
    if (!kmp->prefix_tbl) {
        free(kmp->pattern);
        free(kmp);
        return NULL;
    }
    
    memcpy(kmp->pattern, pattern, len);
    kmp->pattern_len = len;
    
    compute_prefix_tbl(pattern, len, kmp->prefix_tbl, ignore_case);
    
    return kmp;
}

// Free KMP pattern structure
void kmp_free(struct kmp_pattern *kmp)
{
    if (kmp) {
        free(kmp->pattern);
        free(kmp->prefix_tbl);
        free(kmp);
    }
}

// Search for pattern in text, returns position if found or -1 if not found
int kmp_search(struct kmp_pattern *kmp, const unsigned char *text, 
               unsigned int text_len, int ignore_case)
{
    unsigned int i;
    unsigned int q = 0;  // Number of characters matched
    
    for (i = 0; i < text_len; i++) {
        while (q > 0 && (ignore_case ? 
               toupper(kmp->pattern[q]) : kmp->pattern[q]) != 
               (ignore_case ? toupper(text[i]) : text[i])) {
            q = kmp->prefix_tbl[q - 1];
        }
        
        if ((ignore_case ? toupper(kmp->pattern[q]) : kmp->pattern[q]) == 
            (ignore_case ? toupper(text[i]) : text[i])) {
            q++;
        }
        
        if (q == kmp->pattern_len) {
            return i - kmp->pattern_len + 1;
        }
    }
    
    return -1;  // Pattern not found
}

int main(void)
{
    // Test case 1: Basic matching
    const unsigned char *text1 = (unsigned char *)"Hello World! This is a KMP test.";
    const unsigned char *pattern1 = (unsigned char *)"World";
    struct kmp_pattern *kmp1 = kmp_init(pattern1, strlen((char *)pattern1), 0);
    
    printf("Test 1 - Basic matching:\n");
    printf("Text: %s\n", text1);
    printf("Pattern: %s\n", pattern1);
    int pos1 = kmp_search(kmp1, text1, strlen((char *)text1), 0);
    if (pos1 >= 0) {
        printf("Pattern found at position: %d\n", pos1);
    } else {
        printf("Pattern not found\n");
    }
    printf("\n");
    
    // Test case 2: Case-insensitive matching
    const unsigned char *text2 = (unsigned char *)"This is a SAMPLE text";
    const unsigned char *pattern2 = (unsigned char *)"sample";
    struct kmp_pattern *kmp2 = kmp_init(pattern2, strlen((char *)pattern2), 1);
    
    printf("Test 2 - Case-insensitive matching:\n");
    printf("Text: %s\n", text2);
    printf("Pattern: %s\n", pattern2);
    int pos2 = kmp_search(kmp2, text2, strlen((char *)text2), 1);
    if (pos2 >= 0) {
        printf("Pattern found at position: %d\n", pos2);
    } else {
        printf("Pattern not found\n");
    }
    printf("\n");
    
    // Test case 3: Pattern not found
    const unsigned char *text3 = (unsigned char *)"Simple text";
    const unsigned char *pattern3 = (unsigned char *)"missing";
    struct kmp_pattern *kmp3 = kmp_init(pattern3, strlen((char *)pattern3), 0);
    
    printf("Test 3 - Pattern not found:\n");
    printf("Text: %s\n", text3);
    printf("Pattern: %s\n", pattern3);
    int pos3 = kmp_search(kmp3, text3, strlen((char *)text3), 0);
    if (pos3 >= 0) {
        printf("Pattern found at position: %d\n", pos3);
    } else {
        printf("Pattern not found\n");
    }
    
    // Cleanup
    kmp_free(kmp1);
    kmp_free(kmp2);
    kmp_free(kmp3);
    
    return 0;
}
