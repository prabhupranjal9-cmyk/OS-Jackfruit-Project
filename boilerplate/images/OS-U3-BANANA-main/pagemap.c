#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

int main(void) {
    int fd;
    uint64_t value = 0;
    long page_size;
    off_t offset;

    // Allocate one page
    char *p = (char *)malloc(4096);
    if (p == NULL) {
        perror("malloc");
        return 1;
    }

    // Open pagemap
    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd == -1) {
        perror("open");
        free(p);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    // BEFORE touching memory
    offset = ((uintptr_t)p / page_size) * sizeof(uint64_t);

    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        free(p);
        return 1;
    }

    if (read(fd, &value, sizeof(value)) != sizeof(value)) {
        perror("read");
        close(fd);
        free(p);
        return 1;
    }

    printf("Before touching:\n");
    printf("Present bit = %d\n", (int)((value >> 63) & 1));

    // Touch memory (forces page into RAM)
    p[0] = 'A';

    // AFTER touching memory
    offset = ((uintptr_t)p / page_size) * sizeof(uint64_t);

    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        free(p);
        return 1;
    }

    if (read(fd, &value, sizeof(value)) != sizeof(value)) {
        perror("read");
        close(fd);
        free(p);
        return 1;
    }

    printf("After touching:\n");
    printf("Present bit = %d\n", (int)((value >> 63) & 1));

    close(fd);
    free(p);
    return 0;
}
