#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (!environ)
        return 0;
    for (char **e = environ; *e; e++)
        puts(*e);
    return 0;
}
