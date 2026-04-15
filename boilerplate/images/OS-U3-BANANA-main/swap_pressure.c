#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024UL * 1024)

int main(void) {
    // Figure out how much RAM we have and allocate 80% of it
    FILE *f = fopen("/proc/meminfo", "r");
    unsigned long memtotal_kb;
    fscanf(f, "MemTotal: %lu kB", &memtotal_kb);
    fclose(f);
     unsigned long alloc_mb = (memtotal_kb / 1024) * 80 / 100;
    printf("Total RAM: %lu MB, allocating %lu MB (80%%)\n",
           memtotal_kb / 1024, alloc_mb);

    char *region = malloc(alloc_mb * MB);
    if (!region) { perror("malloc"); return 1; }

    // Touch every page so the kernel actually commits physical frames
    printf("Touching all pages...\n");
    for (unsigned long i = 0; i < alloc_mb * MB; i += 4096)
        region[i] = (char)(i & 0xFF);
        printf("Done. Sleeping 5s - run 'vmstat 1' in another terminal.\n");
    printf("PID: %d\n", getpid());
    fflush(stdout);
    sleep(5);

    // Re-read everything - some pages may have been swapped out by now
    printf("Re-reading all pages...\n");
    volatile char sink = 0;
    for (unsigned long i = 0; i < alloc_mb * MB; i += 4096)
        sink += region[i];

    printf("\nMemory status:\n");
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
        "grep -E 'VmRSS|VmSwap' /proc/%d/status", getpid());
        
        system(cmd);

    free(region);
    return 0;
}
