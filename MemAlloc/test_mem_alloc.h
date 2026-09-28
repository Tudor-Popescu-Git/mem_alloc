#pragma once
#include <stddef.h>
#include <stdint.h>

#define MEM_TEST_DEFAULT_SEED        (0x5EED1234ull)
#define MEM_TEST_DEFAULT_STRESS_OPS  ((size_t)200000)

/* Runs the whole suite. Returns the number of failed checks (0 = all passed). */
int mem_test_run_all(uint64_t seed, size_t stress_ops);