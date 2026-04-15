#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MB (1024 * 1024)
#define ALLOC_MB 32   // safe size

int main() {
    // Allocate memory
    char *region = malloc(ALLOC_MB * MB);
    if (region == NULL) {
        perror("malloc");
        return 1;
    }

    // Touch all pages in parent
    for (int i = 0; i < ALLOC_MB * MB; i += 4096) {
        region[i] = 'P';
    }

    printf("Parent: allocated %d MB\n", ALLOC_MB);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        free(region);
        return 1;
    }

    if (pid == 0) {
        // ---- CHILD ----

        // READ (no COW)
        volatile char temp = 0;
        for (int i = 0; i < ALLOC_MB * MB; i += 4096) {
            temp += region[i];
        }

        printf("\nChild READ done (no copy)\n");

        // WRITE (COW happens)
        for (int i = 0; i < ALLOC_MB * MB; i += 4096) {
            region[i] = 'C';
        }

        printf("Child WRITE done (COW triggered)\n");

        free(region);
        _exit(0);
    }

    // ---- PARENT ----
    wait(NULL);
    free(region);

    return 0;
}
