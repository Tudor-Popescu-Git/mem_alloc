# mem_alloc

A small allocator written with AI, written by hand without AI until tag MINE_00


A small first-fit memory allocator written in C11. It manages a fixed,
statically allocated heap (`uint8_t mem_heap[MEM_HEAP_SIZE]`) as a linked
list of blocks, each with a header and a footer.

## API

```c
#include "mem_op.h"

void  mem_init(void);          /* must be called before any other function */
void *mem_alloc(mem_size N);   /* NULL if N > MEM_HEAP_SIZE or no block fits */
void  mem_free(void *ptr);     /* NULL, foreign, interior and double frees are ignored */
```

The header can be included from C and C++.

## How it works

- **Allocation:** first fit. The list is walked from the start, and the first
  free block that is large enough is used. If the leftover part can hold
  another block, the block is split.
- **Free:** the block is merged with a free block before it and/or after it.
  Its payload is then zeroed (`MEM_CLEAR_PAYLOAD`).
- **Validation:** `mem_free` checks that the pointer lies inside the heap and
  that the header watermarks (`0x11111111` / `0x22222222`) are intact before
  it touches the block.
- **End of list:** a zero-size end marker at the end of the heap.

## Alignment

Every pointer returned by `mem_alloc` is aligned to `MEM_ALIGN`, the
strictest alignment of any basic type (`alignof(mem_max_align_t)`). The heap,
the header size and the footer size are all multiples of it, and requests are
rounded up with `MEM_ALIGN_UP`. `static_assert`s in `mem_op.h` fail the build
if the layout ever breaks this.

| Compiler           | `MEM_ALIGN` | Header | Footer | Overhead per block |
|--------------------|-------------|--------|--------|--------------------|
| GCC / Clang, x86-64 | 16          | 32 B   | 32 B   | 64 B               |
| MSVC, x64           | 8           | 32 B   | 24 B   | 56 B               |

`mem_max_align_t` is a union of the basic types. It replaces the standard
`max_align_t`, which MSVC does not declare in C mode.

## Building

CMake 3.10+, C11. `CMakePresets.json` defines two configurations:

| Preset        | Platform          | Compiler |
|---------------|-------------------|----------|
| `linux-debug` | Linux / WSL       | Clang (required; configure fails otherwise) |
| `x64-debug`   | Windows           | MSVC     |

Debug builds enable AddressSanitizer. A missing return value is a compile error
(`-Werror=return-type`, `/we4033 /we4715`).

Without CMake:

```sh
clang -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
      MemAlloc/main.c MemAlloc/mem_op.c MemAlloc/test_mem_alloc.c -o MemAlloc
```

## Running the tests

```sh
./MemAlloc                  # default seed, 200000 stress operations
./MemAlloc 12345            # specific seed
./MemAlloc 12345 2000000    # seed and number of stress operations
```

A failing run prints the exact command that reproduces it.

Compile-time options:

| Define                     | Default     | Effect |
|----------------------------|-------------|--------|
| `TEST_MISUSE=1`            | 0           | also test double free, foreign and interior pointers |
| `MEM_ALIGNMENT=n`          | `MEM_ALIGN` | alignment the tests require of every returned pointer |
| `MEM_DEBUG_PRINT_ENABLE`   | 0           | set to non-zero in `mem_op.h` for debug output on stderr |

Current result: all checks pass on GCC, Clang (ASan + UBSan) and MSVC (ASan).

## Limitations

- Fixed 512-byte heap (`MEM_HEAP_SIZE`); memory is never obtained from the OS.
- Not thread-safe.
- Invalid frees are ignored silently rather than reported.
- A header whose watermarks are intact but whose `size` field was overwritten
  is not detected.

## Repository layout

```
MemAlloc/mem_op.h, mem_op.c        allocator
MemAlloc/test_mem_alloc.c/.h       test suite
MemAlloc/main.c                    test runner entry point
scripts/                           memory-dump helpers (Windows: PowerShell, cdb, Excel export)
```