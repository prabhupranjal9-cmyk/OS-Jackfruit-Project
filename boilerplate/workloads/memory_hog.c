#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int mb = 100;
    if (argc > 1) mb = atoi(argv[1]);

    char *mem = malloc(mb * 1024 * 1024);
    if (!mem) return 1;

    while(1) {
        sleep(1);
    }
}
