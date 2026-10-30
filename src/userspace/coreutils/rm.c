#include "util.h"
#include <unistd.h>

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: rm <path>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0 && rmdir(argv[i]) < 0) {
            cu_perror("rm", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
