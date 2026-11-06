#include <unistd.h>
#include <stdio.h>

int main(void) {
    if (fork() > 0) {
        // Child
        usleep(100000);
        printf("Hello from child\n");
        return 0;
    }

    // Parent
    printf("Hello from parent\n");
    return 0;
}
