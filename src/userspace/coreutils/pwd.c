#include "util.h"
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    char buf[512];

    if (!getcwd(buf, sizeof(buf))) {
        cu_perror("pwd", NULL);
        return 1;
    }
    puts(buf);
    return 0;
}
