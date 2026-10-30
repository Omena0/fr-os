#include <stdio.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname u;
    int all = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'a';

    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    if (all)
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release,
               u.version, u.machine);
    else
        printf("%s\n", u.sysname);
    return 0;
}
