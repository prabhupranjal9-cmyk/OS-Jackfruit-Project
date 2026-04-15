#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1024UL * 1024)
#define PAGE 4096

int main(int argc, char *argv[]) {
    unsigned long total_mb  = argc > 1 ? atol(argv[1]) : 256;
    unsigned long stride_mb = argc > 2 ? atol(argv[2]) : 64;
    int iterations = argc > 3 ? atoi(argv[3]) : 5;
     unsigned long total_bytes  = total_mb * MB;
    unsigned long stride_bytes = stride_mb * MB;

    char *region = malloc(total_bytes);
    if (!region) { perror("malloc"); return 1; }
    memset(region, 0, total_bytes);  // fault everything in

    printf("Total region: %lu MB, working set: %lu MB, iterations: %d\n\n",
           total_mb, stride_mb, iterations);

    // Only access the first stride_bytes repeatedly
    struct timespec start, end;
    for (int iter = 0; iter < iterations; iter++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        volatile char sink = 0;
        for (unsigned long off = 0; off < stride_bytes; off += PAGE)
            sink += region[off];

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec)
                       + (end.tv_nsec - start.tv_nsec) / 1e9;
        printf("  iter %d: %.4f s  (%lu MB scanned)\n",
               iter, elapsed, stride_mb);
    }

    free(region);
    return 0;
}
