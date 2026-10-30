#include <stdio.h>
#include <sys/auxv.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(void)
{
    struct utsname u;
    long pagesz = (long)getauxval(AT_PAGESZ);
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);

    uname(&u);
    printf("machine:  %s\n", u.machine);
    printf("pagesize: %ld\n", pagesz);
    printf("cpus:     %ld\n", nprocs);
    return 0;
}
