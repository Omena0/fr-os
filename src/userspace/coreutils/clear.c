#include <unistd.h>

int main(void)
{
    static const char seq[] = "\033[2J\033[H";
    return write(1, seq, sizeof(seq) - 1) < 0 ? 1 : 0;
}
