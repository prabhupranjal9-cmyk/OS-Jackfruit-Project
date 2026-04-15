#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    void *brk_before = sbrk(0);
    printf("Initial break:       %p\n\n", brk_before);

    // Small allocation → extends heap via brk()
    char *small = malloc(1024);
    printf("After malloc(1KB):   break=%p  ptr=%p  (break moved up)\n",
           sbrk(0), small);
// Large allocation → separate region via mmap() (break should NOT move)
    char *large = malloc(256 * 1024);
    printf("After malloc(256KB): break=%p  ptr=%p  (break unchanged!)\n",
           sbrk(0), large);

    // Another small allocation → still on the heap via brk
    char *small2 = malloc(512);
    printf("After malloc(512B):  break=%p  ptr=%p\n",
           sbrk(0), small2);

    // small and small2 should be close together (both on the heap),
    // but large should be far away (it's in its own mmap region)
    printf("\nsmall  at %p\n", small);
     printf("small2 at %p  (close to small - both on heap)\n", small2);
    printf("large  at %p  (far away - mmap'd separately)\n", large);

    free(small);
    free(small2);
    free(large);
    return 0;
}
