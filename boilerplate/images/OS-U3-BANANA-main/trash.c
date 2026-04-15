#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MB (1024UL * 1024)
#define PAGE 4096

int main(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    unsigned long memtotal_kb;
    fscanf(f, "MemTotal: %lu kB", &memtotal_kb);
    fclose(f);
    unsigned long total_ram_mb = memtotal_kb / 1024;
    printf("Detected RAM: %lu MB\n", total_ram_mb);
    printf("%-12s  %8s  %12s  %10s\n",
           "Alloc(MB)", "Time(s)", "Throughput", "Status");
    printf("-----------------------------------------------------\n");

    // Ramp up: 25%, 50%, 75%, 100%, 130%, 160% of RAM
    int percents[] = {25, 50, 75, 100, 130, 160};
    int num_tests = sizeof(percents) / sizeof(percents[0]);

    for (int t = 0; t < num_tests; t++) {
        unsigned long target_mb = total_ram_mb * percents[t] / 100;
        unsigned long target_bytes = target_mb * MB;

        char *region = malloc(target_bytes);
        if (!region) {
            printf("%-12lu  FAILED (malloc returned NULL)\n", target_mb);
            
             continue;
        }

        // Touch all pages so they're backed by physical frames
        for (unsigned long i = 0; i < target_bytes; i += PAGE)
            region[i] = 1;

        // Random access - this is what kills you.
        // Sequential access lets the kernel prefetch; random access doesn't.
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        unsigned long num_pages = target_bytes / PAGE;
        unsigned long accesses = 100000;
        volatile char sink = 0;
        
         unsigned long seed = 12345;
        for (unsigned long i = 0; i < accesses; i++) {
            seed = seed * 6364136223846793005UL + 1;  // LCG PRNG
            unsigned long page_idx = (seed >> 16) % num_pages;
            sink += region[page_idx * PAGE];
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec)
                       + (end.tv_nsec - start.tv_nsec) / 1e9;
        double throughput = accesses / elapsed;

        const char *status;
        if (elapsed < 1.0)       status = "OK (in-RAM)";
        else if (elapsed < 10.0) status = "SLOW (swapping)";
         else                     status = "THRASHING";

        printf("%-12lu  %8.2f  %10.0f/s  %s\n",
               target_mb, elapsed, throughput, status);

        free(region);
        sleep(2);  // let things settle between runs
    }

    return 0;
}
