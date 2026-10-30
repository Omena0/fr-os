#include "util.h"
#include <sys/stat.h>

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: mkdir <dir>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            cu_perror("mkdir", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
