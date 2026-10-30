#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        printf("%lu\n", (unsigned long)strlen(argv[i]));
    return 0;
}
