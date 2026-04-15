#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_SIZE (1024 * 1024)   // 1 MB
#define BLOCK_SIZE 1024           // 1 KB per block
#define NUM_BLOCKS (POOL_SIZE / BLOCK_SIZE)

int main(void) {
    char *pool = malloc(POOL_SIZE);
    if (!pool) {
        perror("malloc");
        return 1;
    }

    // Track allocated blocks
    int allocated[NUM_BLOCKS];
    memset(allocated, 1, sizeof(allocated));  // mark all as used

    printf("Pool: %d blocks of %d bytes = %d KB total\n",
           NUM_BLOCKS, BLOCK_SIZE, POOL_SIZE / 1024);

    // Free every other block
    int freed = 0;
    for (int i = 0; i < NUM_BLOCKS; i += 2) {
        allocated[i] = 0;
        freed++;
    }

    printf("Freed every other block: %d free blocks (%d KB free)\n",
           freed, (freed * BLOCK_SIZE) / 1024);

    // Find contiguous free blocks
    int need = 128;
    int found = 0;
    int run = 0;

    for (int i = 0; i < NUM_BLOCKS; i++) {
        if (allocated[i] == 0) {
            run++;
            if (run >= need) {
                found = 1;
                break;
            }
        } else {
            run = 0;
        }
    }

    printf("\nLooking for %d contiguous free blocks (%d KB)...\n",
           need, (need * BLOCK_SIZE) / 1024);

    if (found)
        printf("SUCCESS - found contiguous run\n");
    else
        printf("FAILED - %d KB free total, but not contiguous\n",
               (freed * BLOCK_SIZE) / 1024);

    printf("This is EXTERNAL FRAGMENTATION.\n");

    // Internal fragmentation demo
    printf("\n--- Internal Fragmentation ---\n");

    char *tiny1 = malloc(1);
    char *tiny2 = malloc(1);

    printf("malloc(1) addresses: %p and %p\n", tiny1, tiny2);
    printf("Distance between them: %ld bytes\n",
           (long)(tiny2 - tiny1));

    free(tiny1);
    free(tiny2);
    free(pool);

    return 0;
}
