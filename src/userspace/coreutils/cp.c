#include "util.h"
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int in, out, rc = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: cp <source> <dest>\n");
        return 1;
    }
    in = open(argv[1], O_RDONLY);
    if (in < 0) {
        cu_perror("cp", argv[1]);
        return 1;
    }
    out = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) {
        cu_perror("cp", argv[2]);
        close(in);
        return 1;
    }
    if (cu_copy_fd(in, out) < 0) {
        cu_perror("cp", argv[2]);
        rc = 1;
    }
    close(in);
    close(out);
    return rc;
}
