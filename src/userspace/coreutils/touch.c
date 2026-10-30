#include "util.h"
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);

        if (fd < 0) {
            cu_perror("touch", argv[i]);
            rc = 1;
        } else {
            close(fd);
        }
    }
    return rc;
}
