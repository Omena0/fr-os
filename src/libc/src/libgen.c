/*
 * libgen.c — basename() and dirname() for slash-separated paths.
 */
#include <libgen.h>
#include <string.h>

char *basename(char *path)
{
    static char dot[] = ".";
    static char slash[] = "/";
    char *end;

    if (!path || !*path)
        return dot;

    end = path + strlen(path);
    while (end > path && end[-1] == '/')
        end--;

    /* All slashes: the root. */
    if (end == path)
        return slash;

    char *start = end;

    while (start > path && start[-1] != '/')
        start--;
    return start;
}

char *dirname(char *path)
{
    static char dot[] = ".";
    char *end;

    if (!path || !*path)
        return dot;

    end = path + strlen(path);
    while (end > path && end[-1] == '/')
        end--;

    /* No slash in the remaining name. */
    char *slash = end;

    while (slash > path && slash[-1] != '/')
        slash--;

    if (slash == path) {
        if (*path == '/') {
            path[1] = '\0';
            return path;
        }
        return dot;
    }

    /* Drop the component and the slashes before it. */
    char *cut = slash;

    while (cut > path && cut[-1] == '/')
        cut--;
    if (cut == path) {
        if (*path == '/') {
            path[1] = '\0';
            return path;
        }
        return dot;
    }
    *cut = '\0';
    return path;
}
