#include <stdio.h>
#include <string.h>

int main() {
    char buf[5];
    
    // Case 1: 짧은 문자열
    strncpy(buf, "hi", sizeof(buf));
    printf("Case 1: '%s' (length: %zu)\n", buf, strlen(buf));
    
    // Case 2: 정확히 같은 길이
    strncpy(buf, "hello", sizeof(buf));
    printf("Case 2: ");
    for(int i = 0; i < 5; i++) printf("%c", buf[i]);
    printf(" (no null terminator!)\n");
    
    return 0;
} 