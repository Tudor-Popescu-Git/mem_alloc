#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "test_mem_alloc.h"

#if defined(__clang__)
#define MEM_COMPILER "clang " __clang_version__
#elif defined(__GNUC__)
#define MEM_COMPILER "gcc " __VERSION__
#elif defined(_MSC_VER)
#define MEM_COMPILER "msvc"
#else
#define MEM_COMPILER "unknown"
#endif

/*
 * Usage:
 *     MemAlloc                  default seed, 200000 stress ops
 *     MemAlloc 12345            specific seed
 *     MemAlloc 12345 2000000    seed and stress op count
 */
int main(int argc, char** argv)
{
    uint64_t seed = MEM_TEST_DEFAULT_SEED;
    size_t   stress_ops = MEM_TEST_DEFAULT_STRESS_OPS;
    int      failures;

    if (argc > 1) seed = strtoull(argv[1], NULL, 0);
    if (argc > 2) stress_ops = (size_t)strtoull(argv[2], NULL, 0);

    printf("built with %s\n", MEM_COMPILER);

    failures = mem_test_run_all(seed, stress_ops);

    if (failures) {
        printf("reproduce: %s %llu %zu\n",
            argv[0], (unsigned long long)seed, stress_ops);
    }
    return failures ? 1 : 0;
}