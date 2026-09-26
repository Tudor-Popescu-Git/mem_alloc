/*
 * test_mem_alloc.c - tests for a linked-list allocator over a static array.
 *
 * COMPILE AS C. Your allocator is C, so build this as C too: name it .c, or
 * pass /Tc (MSVC) / -x c (gcc, clang). It also compiles as C++ if you ever
 * want that - set MEM_ALLOCATOR_IS_C=1 so the linker finds your C symbols.
 *
 * The point of this file is to make a broken allocator CRASH, at the fault,
 * rather than quietly hand back bad memory. Every allocated block is written
 * end to end, and a free-list traversal is forced after most operations, so
 * a stomped header or a bad next-pointer faults right where it happened
 * instead of thousands of operations later. Run it under the debugger (F5)
 * or with AddressSanitizer and read the top stack frame.
 *
 * RUN
 *     test_mem_alloc.exe                  default seed, 200000 stress ops
 *     test_mem_alloc.exe 12345            specific seed
 *     test_mem_alloc.exe 12345 2000000    seed and stress op count
 *
 * A failing run prints the exact command line that reproduces it.
 *
 * ASAN (strongly recommended for this file)
 *     Project Properties > C/C++ > General > Enable Address Sanitizer = Yes.
 *     On its own it cannot see inside your 256-byte array. See the
 *     MEM_ASAN notes at the bottom for the three poisoning calls that make
 *     it turn every out-of-block read or write into a reported crash.
 *
 * ASSUMPTIONS - read these, they decide whether results mean anything
 *
 *   1. mem_init() fully resets the heap and may be called more than once.
 *      Almost every test starts with it. If yours only works on the first
 *      call, every test after the first will fail for that reason alone.
 *
 *   2. Nothing here knows your heap size or header size. Capacity is
 *      measured at runtime, so the suite keeps working when you change them.
 *
 *   3. If you have your own consistency checker (walk the blocks, walk the
 *      free list, check sizes add up), define MEM_VALIDATE to call it and it
 *      will run after every operation in the stress test:
 *          #define MEM_VALIDATE() my_heap_check()
 *      It should return nonzero when the heap is consistent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "mem_op.h"

/* ---- your allocator ----------------------------------------------------- */

/*
 * Declared here so the file does not depend on your header's name. If you
 * would rather include your header, delete this block and include it.
 *
 * Set MEM_ALLOCATOR_IS_C to 1 if your allocator lives in a .c file and this
 * file is built as .cpp. Wrong setting = "unresolved external symbol" at link.
 */
#ifndef MEM_ALLOCATOR_IS_C
#define MEM_ALLOCATOR_IS_C 0
#endif

#if defined(__cplusplus) && MEM_ALLOCATOR_IS_C
extern "C" {
#endif
void  mem_init(void);
void *mem_alloc(size_t size);
void  mem_free(void *ptr);
#if defined(__cplusplus) && MEM_ALLOCATOR_IS_C
}
#endif

/* ---- configuration ------------------------------------------------------ */

/* Alignment every returned pointer must meet. Lower it to match your design. */
#ifndef MEM_ALIGNMENT
#define MEM_ALIGNMENT 1
#endif

/* Your static array's size, from your header: #define MEM_HEAP_SIZE (256UL) */
#ifndef MEM_HEAP_SIZE
#define MEM_HEAP_SIZE (256UL)
#endif

/* Ceiling for the capacity search. A request past your heap size must fail,
   so probing a little above it is enough and keeps the search fast. */
#ifndef MEM_PROBE_MAX
#define MEM_PROBE_MAX ((size_t)MEM_HEAP_SIZE * 2)
#endif

/* Your consistency checker, if you have one. Must return nonzero when OK. */
#ifndef MEM_VALIDATE
#define MEM_VALIDATE() 1
#endif

/*
 * Misuse tests (double free, freeing foreign or interior pointers) are off by
 * default. What those should do is your decision. Turn this on once you
 * have decided, and edit the assertions in that section to match.
 */
#ifndef TEST_MISUSE
#define TEST_MISUSE 0
#endif

/* Upper bound on simultaneously live blocks. A 256-byte heap yields at most
   a couple of dozen, so this is generous; raise it only if you enlarge the heap. */
#define MAX_LIVE 512

/* ==================================================================== harness */

static int         g_checks;
static int         g_failures;
static int         g_test_failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            g_test_failures++;                                              \
            printf("\n    FAIL line %d: ", __LINE__);                       \
            printf(__VA_ARGS__);                                            \
        }                                                                   \
    } while (0)

/* Same as CHECK, but abandons the test. Use when carrying on would only
   produce noise, such as dereferencing a pointer that came back NULL. */
#define REQUIRE(cond, ...)                                                  \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            g_test_failures++;                                              \
            printf("\n    FAIL line %d: ", __LINE__);                       \
            printf(__VA_ARGS__);                                            \
            printf("  [test abandoned]");                                   \
            return;                                                         \
        }                                                                   \
    } while (0)

static void run_test(const char *name, void (*fn)(void))
{
    g_test_failures = 0;
    printf("  %-36s", name);
    fflush(stdout);
    fn();
    printf(g_test_failures ? "\n" : "ok\n");
}

#define RUN(fn) run_test(#fn, fn)

/* ======================================================= deterministic PRNG */

/*
 * Not rand(). rand() differs between runtimes, so a seed that reproduces a
 * failure in a Debug build might not reproduce in Release. xorshift64* is
 * defined by these few lines, so a seed means the same thing everywhere.
 */
static uint64_t g_rng = 1;

static void rng_seed(uint64_t s)
{
    g_rng = s ? s : 0x9E3779B97F4A7C15ull;
}

static uint64_t rng_next(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

/* Uniform in [0, n). */
static size_t rng_below(size_t n)
{
    return n ? (size_t)(rng_next() % n) : 0;
}

/* ============================================================ measuring helpers */

/*
 * Largest single request a freshly initialised heap can satisfy, found by
 * bisection. Calls mem_init(), so never use it in the middle of a test whose
 * point is to see what state the heap is left in - call it first and keep
 * the number.
 */
static size_t fresh_capacity(void)
{
    size_t lo = 0, hi = MEM_PROBE_MAX;

    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        void  *p;

        mem_init();
        p = mem_alloc(mid);
        if (p) {
            mem_free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    mem_init();
    return lo;
}

/* Largest request the heap can satisfy right now, WITHOUT resetting it.
   Leaves the heap as it found it. */
static size_t current_capacity(size_t upper)
{
    size_t lo = 0, hi = upper;

    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        void  *p = mem_alloc(mid);
        if (p) {
            mem_free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static void  *g_ptrs[MAX_LIVE];   /* scratch array, too big for the stack */

/* Allocate blocks of `size` into g_ptrs until the heap refuses.
   Does not reset the heap first. Returns how many were allocated. */
static size_t fill(size_t size)
{
    size_t n = 0;
    void  *p;

    while (n < MAX_LIVE)
    {
        if ((p = mem_alloc(size)) == NULL)
        {
            break;
        }
        if (n == 2)
        {
            volatile int x = 0;
            x = x + 1;
        }
        g_ptrs[n++] = p;
    }
    return n;
}

static void free_all(size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (g_ptrs[i]) mem_free(g_ptrs[i]);
        g_ptrs[i] = NULL;
    }
}

/* ============================================================ shadow model */

/*
 * Independent record of what the allocator has handed out. Each block is
 * filled with its own byte on allocation and checked before it is freed.
 * If the allocator writes a header into someone else's block, or hands out
 * two overlapping blocks, the fill byte will be wrong.
 */
typedef struct {
    unsigned char *p;
    size_t         size;
    unsigned char  fill;
} live_block;

static live_block g_live[MAX_LIVE];
static size_t     g_live_count;

static void shadow_reset(void)
{
    g_live_count = 0;
}

static int overlaps_live(const unsigned char *p, size_t size)
{
    size_t i;
    size_t pn = size ? size : 1;

    for (i = 0; i < g_live_count; i++) {
        const unsigned char *q = g_live[i].p;
        size_t qn = g_live[i].size ? g_live[i].size : 1;
        if (p < q + qn && q < p + pn) return 1;
    }
    return 0;
}

static void shadow_add(void *p, size_t size)
{
    unsigned char fill = (unsigned char)(rng_next() | 1u);   /* never 0 */

    g_live[g_live_count].p    = (unsigned char *)p;
    g_live[g_live_count].size = size;
    g_live[g_live_count].fill = fill;
    g_live_count++;
    if (size) memset(p, fill, size);
}

/* Returns 1 if the block still holds its fill byte. */
static int shadow_check(size_t index)
{
    const live_block *b = &g_live[index];
    size_t j;

    for (j = 0; j < b->size; j++) {
        if (b->p[j] != b->fill) {
            CHECK(0, "block %p (size %zu) corrupted at offset %zu: "
                     "expected %02X, found %02X",
                  (void *)b->p, b->size, j, b->fill, b->p[j]);
            return 0;
        }
    }
    return 1;
}

static int shadow_check_all(void)
{
    size_t i;
    int ok = 1;
    for (i = 0; i < g_live_count; i++) ok &= shadow_check(i);
    return ok;
}

/* Check a block's contents, free it, and drop it from the model. */
static void shadow_free(size_t index)
{
    void *p = g_live[index].p;
    shadow_check(index);
    g_live[index] = g_live[--g_live_count];
    mem_free(p);
}

static void shadow_free_all(void)
{
    while (g_live_count) shadow_free(g_live_count - 1);
}

/* ============================================================== basic */

static void test_init_then_alloc(void)
{
    unsigned char *p;

    mem_init();
    p = (unsigned char *)mem_alloc(32);
    REQUIRE(p != NULL, "mem_alloc(32) returned NULL on a freshly initialised heap");

    memset(p, 0xA5, 32);
    CHECK(p[0] == 0xA5 && p[31] == 0xA5, "block is not writable end to end");
    mem_free(p);
}

static void test_alloc_zero(void)
{
    void *p, *q;

    mem_init();
    p = mem_alloc(0);

    /*
     * Both NULL and a unique non-NULL pointer are legitimate for size 0.
     * What is not legitimate is the same pointer twice.
     */
    if (p == NULL) {
        printf("(returns NULL) ");
        return;
    }
    q = mem_alloc(0);
    CHECK(q == NULL || q != p, "two mem_alloc(0) calls both returned %p", p);
    if (q) mem_free(q);
    mem_free(p);
}

static void test_free_null(void)
{
    mem_init();
    mem_free(NULL);            /* must not crash */
    CHECK(mem_alloc(16) != NULL, "heap unusable after mem_free(NULL)");
}

static void test_alignment(void)
{
    size_t size, n = 0;

    mem_init();
    for (size = 1; size <= 64 && n < MAX_LIVE; size++) {
        void *p = mem_alloc(size);
        if (!p) break;
        CHECK((uintptr_t)p % MEM_ALIGNMENT == 0,
              "mem_alloc(%zu) returned %p, not %zu-byte aligned",
              size, p, (size_t)MEM_ALIGNMENT);
        g_ptrs[n++] = p;
    }
    REQUIRE(n > 0, "could not allocate even 1 byte");
    free_all(n);
}

static void test_no_overlap(void)
{
    size_t i;

    mem_init();
    shadow_reset();
    rng_seed(1);

    for (i = 0; i < 64; i++) {
        size_t size = 1 + (i % 48);
        void  *p = mem_alloc(size);
        if (!p) break;
        CHECK(!overlaps_live((unsigned char *)p, size),
              "mem_alloc(%zu) returned %p, overlapping a live block", size, p);
        shadow_add(p, size);
    }
    REQUIRE(g_live_count >= 2, "only %zu blocks fit", g_live_count);

    shadow_check_all();
    shadow_free_all();
}

/* ============================================================ capacity */

static void test_largest_allocation(void)
{
    size_t max = fresh_capacity();
    void  *p;

    REQUIRE(max > 0, "no request size succeeded at all");
    printf("(%zu bytes) ", max);

    p = mem_alloc(max);
    CHECK(p != NULL, "mem_alloc(%zu) failed, though it is the measured maximum", max);
    if (p) mem_free(p);

    mem_init();
    p = mem_alloc(max + 1);
    CHECK(p == NULL, "mem_alloc(%zu) succeeded, one byte past the maximum", max + 1);
    if (p) mem_free(p);
}

static void test_huge_requests(void)
{
    void *p;

    mem_init();

    p = mem_alloc((size_t)-1);
    CHECK(p == NULL, "mem_alloc(SIZE_MAX) returned %p. Check for size + header "
                     "overflowing and wrapping round to a small number.", p);

    p = mem_alloc((size_t)-1 - 7);
    CHECK(p == NULL, "mem_alloc(SIZE_MAX - 7) returned %p. Rounding up to "
                     "alignment probably wrapped round.", p);

    p = mem_alloc(MEM_PROBE_MAX);
    CHECK(p == NULL, "mem_alloc(%zu) returned %p", (size_t)MEM_PROBE_MAX, p);

    p = mem_alloc(16);
    CHECK(p != NULL, "heap unusable after refusing oversized requests");
    if (p) mem_free(p);
}

static void test_exhaustion_and_recovery(void)
{
    size_t n;
    void  *p;

    mem_init();
    n = fill(32);
    REQUIRE(n > 0, "could not allocate one 32-byte block");
    REQUIRE(n < MAX_LIVE, "heap never ran out after %d blocks", MAX_LIVE);
    printf("(%zu x 32 bytes) ", n);

    p = mem_alloc(32);
    CHECK(p == NULL, "allocation succeeded after the heap reported full");
    if (p) mem_free(p);

    mem_free(g_ptrs[n - 1]);
    g_ptrs[n - 1] = mem_alloc(32);
    CHECK(g_ptrs[n - 1] != NULL,
          "freeing a block did not make a block of the same size available");

    free_all(n);
}

/* ================================================ reclamation and coalescing */

static void test_fill_free_refill(void)
{
    size_t first, second;

    mem_init();
    first = fill(32);
    REQUIRE(first > 1, "only %zu blocks fit", first);
    free_all(first);

    second = fill(32);
    CHECK(second == first,
          "capacity changed across one fill/free cycle: %zu then %zu blocks",
          first, second);
    free_all(second);
}

static void test_no_drift_over_many_cycles(void)
{
    size_t baseline, cycle;

    /* One mem_init for the whole test. Resetting between cycles would hide
       exactly the slow loss this test exists to find. */
    mem_init();
    baseline = fill(24);
    REQUIRE(baseline > 1, "only %zu blocks fit", baseline);
    free_all(baseline);

    for (cycle = 1; cycle <= 500; cycle++) {
        size_t n = fill(24), i;

        if (n != baseline) {
            CHECK(0, "capacity drifted from %zu to %zu blocks after %zu cycles",
                  baseline, n, cycle);
            free_all(n);
            return;
        }
        /* Alternate free order so both directions of merging get used. */
        if (cycle % 2) {
            free_all(n);
        } else {
            for (i = n; i > 0; i--) mem_free(g_ptrs[i - 1]);
        }
    }
}

static void test_free_order(void)
{
    static const char *names[] = { "reverse", "forward", "shuffled" };
    size_t max = fresh_capacity();
    int pass;

    for (pass = 0; pass < 3; pass++) {
        size_t n, i, after;

        mem_init();
        n = fill(16);
        REQUIRE(n > 3, "only %zu blocks fit", n);

        if (pass == 0) {
            for (i = n; i > 0; i--) mem_free(g_ptrs[i - 1]);
        } else if (pass == 1) {
            for (i = 0; i < n; i++) mem_free(g_ptrs[i]);
        } else {
            rng_seed(0xC0FFEE);
            for (i = n; i > 1; i--) {                 /* Fisher-Yates */
                size_t j = rng_below(i);
                void  *t = g_ptrs[i - 1];
                g_ptrs[i - 1] = g_ptrs[j];
                g_ptrs[j] = t;
            }
            for (i = 0; i < n; i++) mem_free(g_ptrs[i]);
        }

        /* No reset: the heap must be back to one region on its own. */
        after = current_capacity(max);
        CHECK(after == max,
              "%s free order left the heap able to satisfy only %zu of %zu "
              "bytes. Adjacent free blocks were not all merged.",
              names[pass], after, max);
    }
}

static void test_checkerboard(void)
{
    size_t max = fresh_capacity();
    size_t n, i, after;
    void  *p;

    n = fill(32);
    if (n == 2)
    {
        volatile int x = 0;
        x = x + 1;
    }
    REQUIRE(n >= 4, "only %zu blocks fit", n);

    /* Free every other block. No two free blocks are adjacent now. */
    for (i = 0; i < n; i += 2) {
        mem_free(g_ptrs[i]);
        g_ptrs[i] = NULL;
    }

    /* The largest hole is 32 bytes plus at most one header's worth of
       rounding. Twice that cannot fit unless non-adjacent holes were
       merged, which would mean handing out memory that is still in use. */
    p = mem_alloc(64 + 32);
    CHECK(p == NULL, "a 96-byte block fit where only 32-byte holes exist. "
                     "Non-adjacent free blocks were merged.");
    if (p) mem_free(p);

    p = mem_alloc(32);
    CHECK(p != NULL, "a 32-byte request failed although 32-byte holes exist");
    if (p) mem_free(p);

    /* Free the rest. Every hole is now next to another, so all must merge. */
    for (i = 1; i < n; i += 2) {
        mem_free(g_ptrs[i]);
        g_ptrs[i] = NULL;
    }

    after = current_capacity(max);
    CHECK(after == max,
          "after freeing everything, the heap satisfies only %zu of %zu bytes",
          after, max);
}

static void test_split_remainder(void)
{
    size_t max  = fresh_capacity();
    size_t tail = max / 4;
    void  *big, *rest;

    REQUIRE(max > 32, "largest allocation is only %zu bytes", max);

    /* Take most of the heap, leaving a tail a quarter its size. That tail,
       minus one header, must still be handed out - a 1-byte request cannot
       be too big for it. If the split drops the tail, this fails. */
    big = mem_alloc(max - tail);
    REQUIRE(big != NULL, "mem_alloc(%zu) failed", max - tail);

    rest = mem_alloc(1);
    CHECK(rest != NULL, "the tail left after splitting a large block is not "
                        "allocatable. The split is losing it.");
    if (rest) mem_free(rest);
    mem_free(big);

    CHECK(current_capacity(max) == max,
          "heap does not return to full capacity after a split and two frees");
}

static void test_exact_fit_repeated(void)
{
    size_t max = fresh_capacity();
    size_t i;

    /* If an exact fit leaves a zero-size fragment behind, capacity shrinks
       each time and this fails within a few iterations. */
    for (i = 0; i < 200; i++) {
        void *p = mem_alloc(max);
        if (!p) {
            CHECK(0, "exact-fit allocation of %zu bytes failed on iteration %zu",
                  max, i);
            return;
        }
        mem_free(p);
    }
}

static void test_near_exact_fit(void)
{
    size_t max = fresh_capacity();
    size_t shortfall;

    /*
     * Request slightly less than the whole heap. The remainder is too small
     * to hold a header, so the allocator must either hand over the extra or
     * refuse to split. Either is fine; losing the remainder is not.
     */
    for (shortfall = 1; shortfall <= 64 && shortfall < max; shortfall++) {
        void *p = mem_alloc(max - shortfall);
        if (!p) {
            CHECK(0, "mem_alloc(%zu) failed though %zu fits", max - shortfall, max);
            return;
        }
        mem_free(p);
        if (current_capacity(max) != max) {
            CHECK(0, "capacity dropped after allocating and freeing %zu bytes "
                     "(%zu short of full). The unsplittable remainder was lost.",
                  max - shortfall, shortfall);
            return;
        }
    }
}

/* ================================================================ integrity */

static void test_neighbours_untouched(void)
{
    size_t i, half;

    mem_init();
    shadow_reset();
    rng_seed(0xD1CE);

    for (i = 0; i < 300 && g_live_count < MAX_LIVE; i++) {
        size_t size = 1 + rng_below(24);
        void  *p = mem_alloc(size);
        if (!p) break;
        CHECK(!overlaps_live((unsigned char *)p, size),
              "mem_alloc(%zu) returned %p, overlapping a live block", size, p);
        shadow_add(p, size);
    }
    REQUIRE(g_live_count >= 4, "only %zu blocks fit", g_live_count);
    shadow_check_all();

    /* Free half at random. Freeing must not touch the survivors. */
    half = g_live_count / 2;
    while (g_live_count > half) shadow_free(rng_below(g_live_count));
    shadow_check_all();

    /* Refill the holes. New blocks must not overwrite the survivors either. */
    for (i = 0; i < 300 && g_live_count < MAX_LIVE; i++) {
        size_t size = 1 + rng_below(24);
        void  *p = mem_alloc(size);
        if (!p) break;
        CHECK(!overlaps_live((unsigned char *)p, size),
              "reused block %p (size %zu) overlaps a live block", p, size);
        shadow_add(p, size);
    }
    shadow_check_all();
    shadow_free_all();
}

/* ============================================================ crash hunting

   These are deliberately adversarial. They only ever touch memory the
   allocator handed them, so if any of them crashes, the fault is in the
   allocator, not the test. A crash here is the goal: it means a real bug
   surfaced loudly. Run under the debugger or ASan to see the exact line. */

/* Force a full free-list traversal. A single-byte alloc has to walk from the
   list head, so a corrupted next-pointer dereferences garbage HERE, right
   after the operation that corrupted it, instead of much later. */
static void force_walk(void)
{
    void *p = mem_alloc(1);
    if (p) mem_free(p);
}

static void test_boundary_write_every_size(void)
{
    size_t max = fresh_capacity();
    size_t size;

    /* For every legal size: take a block, write all of it, hand it back.
       If usable space is even one byte short of what was promised, this
       write lands in the next block's header and the next traversal dies. */
    for (size = 1; size <= max; size++) {
        unsigned char *p = (unsigned char *)mem_alloc(size);
        REQUIRE(p != NULL, "mem_alloc(%zu) failed though %zu is the maximum",
                size, max);
        memset(p, 0xFF, size);   /* the whole block, to the last byte */
        force_walk();            /* crashes here if that write hit a header */
        mem_free(p);
        force_walk();
    }
}

static void test_neighbour_stomp(void)
{
    size_t i, n;

    /* Fill the heap, write every block to its last byte, then walk. If any
       block's payload overruns into the adjacent block's header, the walk
       follows a smashed next-pointer off into nowhere. */
    mem_init();
    n = 0;
    for (;;) {
        unsigned char *p = (unsigned char *)mem_alloc(8);
        if (!p) break;
        memset(p, 0xAB, 8);
        g_ptrs[n++] = p;
        if (n >= MAX_LIVE) break;
    }
    REQUIRE(n >= 2, "only %zu blocks fit", n);

    force_walk();                    /* whole list must still be walkable */
    for (i = 0; i < n; i++) {        /* free every other order of neighbours */
        mem_free(g_ptrs[i]);
        force_walk();                /* coalescing must not run off an edge */
    }
}

static void test_fragmentation_storm(void)
{
    size_t op;

    /* Random alloc/free, every block written full, a forced walk after each
       step. This is the mode most likely to expose a corrupted free list,
       because it keeps the heap fragmented and traverses constantly. */
    mem_init();
    shadow_reset();
    rng_seed(0xBADC0DE);

    for (op = 0; op < 20000; op++) {
        int alloc = g_live_count == 0 ? 1
                  : g_live_count >= MAX_LIVE - 1 ? 0
                  : (int)(rng_below(2));

        if (alloc) {
            size_t size = 1 + rng_below(40);
            void  *p = mem_alloc(size);
            if (!p) continue;
            if (overlaps_live((unsigned char *)p, size)) {
                CHECK(0, "op %zu: mem_alloc(%zu) returned %p over a live block",
                      op, size, p);
                return;
            }
            shadow_add(p, size);     /* writes the full block */
        } else {
            shadow_free(rng_below(g_live_count));
            if (g_test_failures) return;
        }
        force_walk();                /* the crash, if any, lands here */
    }
    shadow_free_all();
}

static void test_alloc_free_same_slot(void)
{
    size_t max = fresh_capacity();
    size_t i;

    /* Hammer one slot: full-heap alloc, full write, free, repeat. Exercises
       the exact-fit and whole-heap-coalesce paths thousands of times, where
       a one-off pointer error tends to hide. */
    for (i = 0; i < 5000; i++) {
        unsigned char *p = (unsigned char *)mem_alloc(max);
        REQUIRE(p != NULL, "iteration %zu: mem_alloc(%zu) failed", i, max);
        memset(p, (int)(i & 0xFF), max);
        mem_free(p);
        force_walk();
    }
}

/* ================================================================ misuse */

#if TEST_MISUSE
/* Edit these to match what you decide the allocator should do. As written
   they only assert that the heap survives. */

static void test_double_free(void)
{
    void *p;
    mem_init();
    p = mem_alloc(32);
    REQUIRE(p != NULL, "mem_alloc(32) failed");
    mem_free(p);
    mem_free(p);
    CHECK(mem_alloc(32) != NULL, "heap unusable after a double free");
}

static void test_free_foreign(void)
{
    int local = 0;
    mem_init();
    mem_free(&local);
    CHECK(mem_alloc(32) != NULL, "heap unusable after freeing a stack pointer");
}

static void test_free_interior(void)
{
    unsigned char *p;
    mem_init();
    p = (unsigned char *)mem_alloc(64);
    REQUIRE(p != NULL, "mem_alloc(64) failed");
    mem_free(p + 16);
    CHECK(mem_alloc(32) != NULL, "heap unusable after freeing an interior pointer");
}
#endif

/* ================================================================ stress */

static uint64_t g_seed       = 0x5EED1234;
static size_t   g_stress_ops = 200000;

static void test_random_stress(void)
{
    size_t max = fresh_capacity();
    size_t op, allocs = 0, frees = 0, refused = 0;

    shadow_reset();
    rng_seed(g_seed);

    for (op = 0; op < g_stress_ops; op++) {
        int do_alloc;

        if (g_live_count == 0)                 do_alloc = 1;
        else if (g_live_count >= MAX_LIVE - 1) do_alloc = 0;
        else                                   do_alloc = rng_below(100) < 55;

        if (do_alloc) {
            /* Mostly small requests, occasionally a large one. */
            size_t size = rng_below(100) < 85 ? 1 + rng_below(64)
                                              : 1 + rng_below(1024);
            void  *p = mem_alloc(size);

            if (!p) { refused++; continue; }

            if (overlaps_live((unsigned char *)p, size)) {
                CHECK(0, "op %zu: mem_alloc(%zu) returned %p, overlapping a "
                         "live block", op, size, p);
                return;
            }
            if ((uintptr_t)p % MEM_ALIGNMENT) {
                CHECK(0, "op %zu: mem_alloc(%zu) returned misaligned %p",
                      op, size, p);
                return;
            }
            shadow_add(p, size);
            allocs++;
        } else {
            size_t idx = rng_below(g_live_count);
            if (!shadow_check(idx)) {
                printf("\n    (at op %zu)", op);
                return;
            }
            shadow_free(idx);
            frees++;
        }

        if (!MEM_VALIDATE()) {
            CHECK(0, "op %zu: MEM_VALIDATE reported an inconsistent heap", op);
            return;
        }

        /* Full sweep every 1024 ops: catches corruption close to its cause
           without making every operation O(live blocks). */
        if ((op & 0x3FF) == 0 && !shadow_check_all()) {
            printf("\n    (detected at op %zu)", op);
            return;
        }
    }

    shadow_check_all();
    shadow_free_all();
    printf("(%zu allocs, %zu frees, %zu refused) ", allocs, frees, refused);

    CHECK(current_capacity(max) == max,
          "after freeing everything the heap satisfies only %zu of %zu bytes",
          current_capacity(max), max);
}

/* ================================================================== main */

int main(int argc, char **argv)
{
    if (argc > 1) g_seed       = strtoull(argv[1], NULL, 0);
    if (argc > 2) g_stress_ops = (size_t)strtoull(argv[2], NULL, 0);

    printf("seed %llu, %zu stress ops, alignment %zu\n\n",
           (unsigned long long)g_seed, g_stress_ops, (size_t)MEM_ALIGNMENT);

    printf("basic\n");
    RUN(test_init_then_alloc);
    RUN(test_alloc_zero);
    RUN(test_free_null);
    RUN(test_alignment);
    RUN(test_no_overlap);

    printf("\ncapacity\n");
    RUN(test_largest_allocation);
    RUN(test_huge_requests);
    RUN(test_exhaustion_and_recovery);

    printf("\nreclamation and coalescing\n");
    RUN(test_fill_free_refill);
    RUN(test_no_drift_over_many_cycles);
    RUN(test_free_order);
    RUN(test_checkerboard);
    RUN(test_split_remainder);
    RUN(test_exact_fit_repeated);
    RUN(test_near_exact_fit);

    printf("\nintegrity\n");
    RUN(test_neighbours_untouched);

    printf("\ncrash hunting\n");
    RUN(test_boundary_write_every_size);
    RUN(test_neighbour_stomp);
    RUN(test_fragmentation_storm);
    RUN(test_alloc_free_same_slot);

#if TEST_MISUSE
    printf("\nmisuse\n");
    RUN(test_double_free);
    RUN(test_free_foreign);
    RUN(test_free_interior);
#endif

    printf("\nstress\n");
    RUN(test_random_stress);

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) {
        printf("reproduce: %s %llu %zu\n",
               argv[0], (unsigned long long)g_seed, g_stress_ops);
    }
    return g_failures ? 1 : 0;
}

/*
 * MEM_ASAN - making AddressSanitizer see inside your static heap array
 *
 * With ASan enabled, add these to your allocator (not to this file):
 *
 *     #include <sanitizer/asan_interface.h>
 *
 *     in mem_init, after setting up the free list:
 *         ASAN_POISON_MEMORY_REGION(mem_heap, sizeof mem_heap);
 *     then unpoison only the headers your code itself reads and writes.
 *
 *     in mem_alloc, just before returning p:
 *         ASAN_UNPOISON_MEMORY_REGION(p, size);
 *
 *     in mem_free, after you have finished touching the block's header:
 *         ASAN_POISON_MEMORY_REGION(ptr, block_payload_size);
 *
 * After that, any read or write through a freed pointer, or past the end of
 * the requested size, stops the program with a stack trace pointing at the
 * offending line. The macros compile to nothing when ASan is off.
 */
