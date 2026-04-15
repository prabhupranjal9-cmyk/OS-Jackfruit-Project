// demand.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024 * 1024)

void show_rss(const char *label) {
    char command[256];

    snprintf(command, sizeof(command),
             "grep Vm /proc/%d/status", getpid());

    printf("\n[%s]\n", label);
    fflush(stdout);

    system(command);
}

int main() {
    show_rss("1. Before malloc");

    char *big = malloc(128 * MB);   // reduced to avoid crash
    if (big == NULL) {
        perror("malloc failed");
        return 1;
    }

    show_rss("2. After malloc");

    memset(big, 1, 64 * MB);
    show_rss("3. After touching 64MB");

    memset(big, 2, 128 * MB);
    show_rss("4. After touching 128MB");

    free(big);
    return 0;
}
