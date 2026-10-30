#include "util.h"
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int rc = 0;
    int i;

    if (argc < 2)
        return cu_copy_fd(0, 1) < 0 ? 1 : 0;

    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);

        if (fd < 0) {
            cu_perror("cat", argv[i]);
            rc = 1;
            continue;
        }
        if (cu_copy_fd(fd, 1) < 0) {
            cu_perror("cat", argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
