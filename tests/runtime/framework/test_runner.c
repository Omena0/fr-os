#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "test_framework.h"

#define ARG_HELP 0
#define ARG_VERBOSE 1
#define ARG_VERSION 2
#define ARG_RUN_EVERYTHING 3

void print_usage(char* prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\n");
    printf("Run the OS runtime tests.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h    Show this help message\n");
    printf("  --verbose, -v Enable verbose output (default: quiet)\n");
    printf("  --version     Show version information\n");
    printf("  --run-everything  Run all tests including edge cases and probes\n");
    printf("\n");
}

void print_version() {
    printf("OS Runtime Test Runner v1.0.0\n");
}

int main(int argc, char* argv[]) {
    int verbose = 0;
    (void)verbose;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (argv[i][0] == '-') {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Include all test suites
    extern struct test_suite test_process_syscalls;
    extern struct test_suite test_memory_syscalls;
    extern struct test_suite test_file_syscalls;
    extern struct test_suite test_ipc_syscalls;
    extern struct test_suite test_scheduling_syscalls;
    extern struct test_suite test_security_syscalls;
    extern struct test_suite test_init_system;
    
    struct test_suite* all_suites[] = {
        &test_process_syscalls,
        &test_memory_syscalls,
        &test_file_syscalls,
        &test_ipc_syscalls,
        &test_scheduling_syscalls,
        &test_security_syscalls,
        &test_init_system,
        NULL
    };
    
    // Statistics
    int total_suites = 0;
    int passed_suites = 0;
    int failed_suites = 0;
    int skipped_suites = 0;
    int total_tests = 0;
    int passed_tests = 0;
    int failed_tests = 0;
    int skipped_tests = 0;
    
    // Run all suites
    for (int i = 0; all_suites[i] != NULL; i++) {
        struct test_suite* suite = all_suites[i];
        total_suites++;
        
        if (verbose) {
            printf("=== Running Suite: %s ===\n", suite->name);
        }
        
        int ret = test_run_suite(suite);
        
        // Use the framework's actual per-test results rather than
        // suite->num_cases, so a suite that fails only some of its
        // cases is not counted entirely as failed.
        int suite_passed = test_get_passed();
        int suite_failed = test_get_failed();
        int suite_skipped = test_get_skipped();
        total_tests += suite_passed + suite_failed + suite_skipped;
        passed_tests += suite_passed;
        failed_tests += suite_failed;
        skipped_tests += suite_skipped;
        
        if (ret == TEST_PASS) {
            passed_suites++;
            if (verbose) {
                printf("=== Suite %s PASSED ===\n\n", suite->name);
            }
        } else if (ret == TEST_FAIL) {
            failed_suites++;
            if (verbose) {
                printf("=== Suite %s FAILED ===\n\n", suite->name);
            }
        } else {
            skipped_suites++;
            if (verbose) {
                printf("=== Suite %s SKIPPED ===\n\n", suite->name);
            }
        }
    }
    
    // Print summary
    printf("\n");
    printf("==================== SUMMARY ====================\n");
    printf("Suites: %d total, %d passed, %d failed, %d skipped\n", 
            total_suites, passed_suites, failed_suites, skipped_suites);
    printf("Tests:  %d total, %d passed, %d failed, %d skipped\n", 
            total_tests, passed_tests, failed_tests, skipped_tests);
    
    if (failed_suites > 0 || failed_tests > 0) {
        return 1; // Exit with error if any tests failed
    }
    
    return 0;
}
