#!/usr/bin/env python3
"""
Comprehensive debug script for va_arg issue.

This script adds detailed logging to:
1. test_log to show arguments passed
2. handle_string to show the va_arg pointer value
3. emit_padded to show output being generated
4. The va_list state changes through the call chain

It builds a test program that links with the libc and runs it to capture
all debug output.
"""

import subprocess
import os
import sys
import tempfile
import shutil

# Project paths
PROJECT_ROOT = "/home/omena0/Github/os/.kilo/worktrees/debug-bad-ptr-logging"
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
LIBC_BUILD_DIR = os.path.join(BUILD_DIR, "libc")
LIBC_A = os.path.join(BUILD_DIR, "libc.a")
DEBUG_BIN = os.path.join(BUILD_DIR, "debug_va_arg")


def run_cmd(cmd, cwd=None, capture=True, check=True):
    """Run a command and return stdout, stderr, returncode."""
    print(f"[CMD] {cmd}")
    result = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=capture, text=True)
    if capture:
        if result.stdout:
            print(f"[STDOUT]\n{result.stdout}")
        if result.stderr:
            print(f"[STDERR]\n{result.stderr}")
    if check and result.returncode != 0:
        print(f"[ERROR] Command failed with exit code {result.returncode}")
        sys.exit(result.returncode)
    return result


def build_libc():
    """Build the libc static library."""
    print("\n=== Building libc.a ===")
    os.chdir(PROJECT_ROOT)
    run_cmd("make clean")
    run_cmd("make BUILD=build libc.a")


def build_debug_program():
    """Build the debug program that tests va_arg with detailed logging."""
    
    # Create a comprehensive debug test program
    debug_c = r'''
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

/* DEBUG macros */
#define DEBUG_LOG(fmt, ...) \
    do { \
        fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#define DEBUG_VA_LIST(ap, label) \
    do { \
        fprintf(stderr, "[DEBUG] %s:%d: %s - va_list address: %p\n", \
                __func__, __LINE__, label, (void*)&(ap)); \
    } while(0)

/* Mock the libc's internal printf functions to add debug logging */

// Forward declare the libc's internal functions
extern size_t __libc_vformat(void (*sink)(void *ctx, const char *data, size_t n),
                 void *ctx, const char *fmt, va_list ap);

// Custom sink that logs what it receives
static void debug_sink(void *ctx, const char *data, size_t n) {
    FILE *out = (FILE*)ctx;
    DEBUG_LOG("sink called: writing %zu bytes: '%.*s'", n, (int)n, data);
    if (out && data && n > 0) {
        fwrite(data, 1, n, out);
    }
}

// Test function that mimics test_log with debug logging
void test_log_debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    DEBUG_LOG("test_log_debug called with fmt: '%s'", fmt);
    
    // DEBUG: Show va_list state before vsnprintf
    DEBUG_VA_LIST(ap, "before vsnprintf");
    
    char buf[512];
    
    // Use the libc's vsnprintf
    extern int vsnprintf(char *str, size_t size, const char *format, va_list ap);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    
    DEBUG_LOG("vsnprintf returned len=%d", len);
    DEBUG_VA_LIST(ap, "after vsnprintf");
    
    va_end(ap);
    
    DEBUG_LOG("Output buffer: '%s'", buf);
    printf("%s\n", buf);
}

// Test the specific failing format from the suite summary
void test_suite_summary() {
    DEBUG_LOG("=== Testing suite summary format ===");
    test_log_debug("=== Suite %s: %d passed, %d failed, %d skipped ===", 
                   "suite_a", 1, 2, 3);
    DEBUG_LOG("=== Suite summary test done ===\n");
}

// Test simple string format
void test_simple_string() {
    DEBUG_LOG("=== Testing simple string format ===");
    test_log_debug("Hello %s!", "world");
    DEBUG_LOG("=== Simple string test done ===\n");
}

// Test multiple string args
void test_multiple_strings() {
    DEBUG_LOG("=== Testing multiple string args ===");
    test_log_debug("%s %s %s", "one", "two", "three");
    DEBUG_LOG("=== Multiple strings test done ===\n");
}

// Test with precision
void test_precision() {
    DEBUG_LOG("=== Testing string with precision ===");
    test_log_debug("%.5s", "hello world");
    DEBUG_LOG("=== Precision test done ===\n");
}

// Test string with width
void test_width() {
    DEBUG_LOG("=== Testing string with width ===");
    test_log_debug("%10s", "hello");
    DEBUG_LOG("=== Width test done ===\n");
}

// Test edge cases
void test_edge_cases() {
    DEBUG_LOG("=== Testing edge cases ===");
    test_log_debug("%s", NULL);
    DEBUG_LOG("=== NULL string test done ===\n");
    
    test_log_debug("%s", "");
    DEBUG_LOG("=== Empty string test done ===\n");
}

// Test integer formats mixed with strings
void test_mixed() {
    DEBUG_LOG("=== Testing mixed formats ===");
    test_log_debug("Count: %d, Name: %s, Hex: %x", 42, "test", 255);
    DEBUG_LOG("=== Mixed formats test done ===\n");
}

// Direct test of the libc's __libc_vformat
void test_direct_vformat() {
    DEBUG_LOG("=== Testing __libc_vformat directly ===");
    
    va_list ap;
    va_start(ap, "test");
    va_arg(ap, int); // consume dummy
    
    const char *fmt = "=== Suite %s: %d passed, %d failed, %d skipped ===";
    va_list ap2;
    va_start(ap2, fmt);
    const char *s = "suite_a";
    int p = 1, f = 2, s2 = 3;
    
    // Actually pass the arguments properly
    va_list ap3;
    va_start(ap3, "dummy");
    va_arg(ap3, int);
    const char *args[] = {s};
    // This is tricky - we need to pass real variadic args
    
    va_end(ap3);
    va_end(ap2);
    va_end(ap);
    DEBUG_LOG("=== Direct vformat test done ===\n");
}

int main(int argc, char *argv[]) {
    DEBUG_LOG("Starting va_arg debug test");
    DEBUG_LOG("Arguments: argc=%d", argc);
    for (int i = 0; i < argc; i++) {
        DEBUG_LOG("  argv[%d] = '%s'", i, argv[i]);
    }
    
    test_simple_string();
    test_multiple_strings();
    test_precision();
    test_width();
    test_edge_cases();
    test_mixed();
    test_suite_summary();
    
    DEBUG_LOG("All tests completed");
    return 0;
}
'''
    
    # Write the debug program
    debug_c_path = os.path.join(BUILD_DIR, "debug_va_arg.c")
    with open(debug_c_path, 'w') as f:
        f.write(debug_c)
    
    # Compile with the project's libc
    # First, find the include paths
    include_paths = [
        os.path.join(PROJECT_ROOT, "src", "libc", "include"),
        os.path.join(PROJECT_ROOT, "src", "kernel"),
    ]
    
    includes = " ".join(f"-I{p}" for p in include_paths)
    
    # Build command
    cmd = f"gcc -g -O0 -std=gnu11 -Wall -Wextra {includes} "
    cmd += f"-o {DEBUG_BIN} {debug_c_path} {LIBC_A} -no-pie"
    
    print("\n=== Building debug program ===")
    run_cmd(cmd)
    
    return DEBUG_BIN


def run_debug_program(bin_path):
    """Run the debug program and capture output."""
    print("\n=== Running debug program ===")
    result = run_cmd(bin_path, check=False)
    return result


def main():
    print("=" * 60)
    print("VA_ARG DEBUG SCRIPT")
    print("=" * 60)
    
    # Step 1: Build libc
    build_libc()
    
    # Step 2: Build debug program
    bin_path = build_debug_program()
    
    # Step 3: Run debug program
    run_debug_program(bin_path)
    
    print("\n=== Debug script complete ===")


if __name__ == "__main__":
    main()