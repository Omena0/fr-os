#include <stdio.h>

static const char *tools[] = {
    "cat", "clear", "cp", "date", "echo", "env", "false", "head", "help",
    "lscpu", "ls", "mem", "mkdir", "pwd", "rm", "sleep", "strlen", "tail",
    "time", "touch", "true", "uname", "uptime", "whoami", NULL
};

int main(void)
{
    puts("Fr OS coreutils:");
    for (int i = 0; tools[i]; i++)
        printf("  %s\n", tools[i]);
    puts("Use 'exit' to leave the shell.");
    return 0;
}
