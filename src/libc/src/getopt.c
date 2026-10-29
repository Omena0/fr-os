/*
 * getopt.c — POSIX getopt and the getopt_long extension.
 *
 * A small, conventional implementation: short options may be bundled, an
 * option argument may be attached (-ovalue) or the next argv element, "--"
 * terminates option parsing, and a leading ':' in optstring selects silent
 * error reporting. getopt_long matches exact names and unique prefixes.
 */
#include <getopt.h>
#include <stdio.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;
int optreset;

static int sp = 1;

int getopt(int argc, char *const argv[], const char *optstring)
{
    if (optreset || optind == 0) {
        optreset = 0;
        optind = 1;
        sp = 1;
    }

    if (sp == 1) {
        if (optind >= argc || argv[optind][0] != '-' ||
            argv[optind][1] == '\0') {
            return -1;
        }
        if (strcmp(argv[optind], "--") == 0) {
            optind++;
            return -1;
        }
    }

    int silent = optstring[0] == ':';
    char c = argv[optind][sp];
    const char *spec = strchr(optstring, c);

    optopt = (unsigned char)c;

    if (c == ':' || !spec) {
        if (opterr && !silent)
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (argv[optind][++sp] == '\0') {
            optind++;
            sp = 1;
        }
        return '?';
    }

    if (spec[1] == ':') {
        int optional = spec[2] == ':';

        if (argv[optind][sp + 1] != '\0') {
            optarg = &argv[optind][sp + 1];
            optind++;
            sp = 1;
        } else if (optional) {
            optarg = NULL;
            optind++;
            sp = 1;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
            sp = 1;
        } else {
            if (opterr && !silent)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                        argv[0], c);
            optind++;
            sp = 1;
            return silent ? ':' : '?';
        }
        return optopt;
    }

    if (argv[optind][++sp] == '\0') {
        optind++;
        sp = 1;
    }
    return optopt;
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    if (optreset || optind == 0) {
        optreset = 0;
        optind = 1;
        sp = 1;
    }

    /* Short options, "-", and the "--" terminator are getopt's business. */
    if (optind >= argc || argv[optind][0] != '-' ||
        argv[optind][1] != '-' || argv[optind][2] == '\0')
        return getopt(argc, argv, optstring);

    const char *name = argv[optind] + 2;
    const char *eq = strchr(name, '=');
    size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
    const struct option *match = NULL;
    int matches = 0;

    for (const struct option *o = longopts; o && o->name; o++) {
        if (strncmp(o->name, name, nlen) == 0) {
            if (strlen(o->name) == nlen) {
                match = o;
                matches = 1;
                break;
            }
            match = o;
            matches++;
        }
    }

    if (matches != 1) {
        if (opterr)
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                    argv[0], (int)nlen, name);
        optind++;
        return '?';
    }

    if (longindex)
        *longindex = (int)(match - longopts);

    const char *arg = eq ? eq + 1 : NULL;

    if (match->has_arg == required_argument && !arg) {
        if (optind + 1 < argc) {
            arg = argv[optind + 1];
            optind++;
        } else {
            if (opterr)
                fprintf(stderr, "%s: option '--%s' requires an argument\n",
                        argv[0], match->name);
            optind++;
            return '?';
        }
    }

    optind++;

    if (match->flag) {
        *match->flag = match->val;
        return 0;
    }

    optarg = (char *)arg;
    return match->val;
}
