#include "util.h"
#include <dirent.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    struct dirent *de;

    if (!d) {
        cu_perror("ls", path);
        return 1;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        printf("%s\n", de->d_name);
    }
    closedir(d);
    return 0;
}
