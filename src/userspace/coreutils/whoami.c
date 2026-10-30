#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("uid=%lu gid=%lu\n", (unsigned long)getuid(), (unsigned long)getgid());
    return 0;
}
