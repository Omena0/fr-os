/*
 * getopt.h — command-line option parsing.
 *
 * POSIX getopt plus the common getopt_long extension. A program that wants
 * options sets optind = 1 (and optreset = 1 after reusing the parser).
 */
#ifndef GETOPT_H
#define GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
extern int optreset;

int getopt(int argc, char *const argv[], const char *optstring);

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#define no_argument        0
#define required_argument  1
#define optional_argument  2

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* GETOPT_H */
