#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
#include <assert.h>

#define MEM_HEAP_SIZE (512UL)
#define MEM_ALLOC_RET_TYPE_OK	((MEM_ALLOC_RET_TYPE)0)
#define MEM_ALLOC_RET_TYPE_NOK	((MEM_ALLOC_RET_TYPE)1)
#define MEM_WATERMARKS
#define MEM_CLEAR_PAYLOAD
#define MEM_DEBUG_PRINT_ENABLE (0)
#define MEM_SIZE_SPECIFIER "zu"
#define MEM_LOC(node)							(*(node))
#define MEM_LINK(node)							((node)->next)
#define MEM_SIZE(node)							((node)->size)
#define MEM_AVAIL(node)							((node)->avail)
#define MEM_FOOTER_ADDR(header)					((mem_footer *)((uint8_t *)(header) + sizeof(*(header)) + ((header)->size * sizeof(mem_word_type))))
#define MEM_WHOLE_SIZE_NODE(node)				(sizeof(mem_header) + sizeof(mem_word_type) * ((node)->size) + sizeof(mem_footer))
#define MEM_WHOLE_SIZE(size)					(sizeof(mem_header) + sizeof(mem_word_type) * (size) + sizeof(mem_footer))
#define MEM_HEADER_SIZE							(sizeof(mem_header))
#define MEM_PROVISIONING(needed_size)			((needed_size) * sizeof(mem_word_type) + sizeof(mem_footer))

#if MEM_DEBUG_PRINT_ENABLE != 0
#define MEM_DEBUG_PRINTF(...) fprintf( stderr, __VA_ARGS__ )
#else
#define MEM_DEBUG_PRINTF(...) do { }while(0)
#endif

typedef union
{
	long double	ld;
	long long	ll;
	double		d;
	void* p;
	void		(*fp)(void);
}	mem_max_align_t;


#define MEM_ALIGN        (alignof(mem_max_align_t))
#define MEM_ALIGN_UP(x)  (((x) / MEM_ALIGN + ((x) % MEM_ALIGN != 0)) * MEM_ALIGN)

typedef int32_t	MEM_ALLOC_RET_TYPE;
typedef size_t		mem_size;
typedef uint8_t		mem_word_type;
typedef uint8_t		mem_avail;
typedef uint8_t		mem_bool;


typedef struct mem_header
{
#ifdef MEM_WATERMARKS
	uint32_t mem_header_start;
#endif
	struct mem_header* next;
	mem_size				size;
	mem_avail				avail;
#ifdef MEM_WATERMARKS
	uint32_t mem_header_end;
#endif
	alignas(mem_max_align_t)
	mem_word_type	start[];
}	mem_header, * mem_header_ptr;

typedef struct mem_footer
{
#ifdef MEM_WATERMARKS
	uint32_t mem_footer_start;
#endif
	alignas(mem_max_align_t)
	mem_size	size;
	mem_avail	avail;
#ifdef MEM_WATERMARKS
	uint32_t mem_footer_end;
#endif
}	mem_footer;


static_assert(sizeof(mem_header) == offsetof(mem_header, start),
	"payload must start right after the header");
static_assert(sizeof(mem_header) % alignof(mem_max_align_t) == 0,
	"header size must be a multiple of the alignment");
static_assert(sizeof(mem_footer) % alignof(mem_max_align_t) == 0,
	"footer size must be a multiple of the alignment");


#if defined(__cplusplus)
extern "C" {
#endif
	void mem_init(void);
	void* mem_alloc(mem_size N);
	void mem_free(void* ptr);

#if defined(__cplusplus)
}
#endif
