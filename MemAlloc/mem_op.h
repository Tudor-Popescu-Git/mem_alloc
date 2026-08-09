#include <stdint.h>

typedef int32_t		MEM_ALLOC_RET_TYPE;

#define MEM_HEAP_SIZE (256UL)
#define MEM_ALLOC_RET_TYPE_OK	((MEM_ALLOC_RET_TYPE)0)
#define MEM_ALLOC_RET_TYPE_NOK	((MEM_ALLOC_RET_TYPE)1)
#define MEM_WATERMARKS
#define MEM_CLEAR_PAYLOAD

typedef uint32_t	mem_size;
typedef uint8_t		mem_word_type;
typedef uint8_t		mem_avail;
typedef uint8_t		mem_bool;

#pragma pack(push, 1)
typedef struct mem_header
{
#ifdef MEM_WATERMARKS
	uint32_t mem_header_start;
#endif
	struct mem_header *		next;
	mem_size				size;
	mem_avail				avail;
#ifdef MEM_WATERMARKS
	uint32_t mem_header_end;
#endif
	mem_word_type	start[];
}	mem_header, *	mem_header_ptr;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct mem_footer
{
#ifdef MEM_WATERMARKS
	uint32_t mem_footer_start;
#endif
	mem_size	size;
	mem_avail	avail;
#ifdef MEM_WATERMARKS
	uint32_t mem_footer_end;
#endif
}	mem_footer;
#pragma pack(pop)

#define MEM_LOC(node)							(*(node))
#define MEM_LINK(node)							(mem_header *)((node)->next)
#define MEM_SIZE(node)							((node)->size)
#define MEM_AVAIL(node)							((node)->avail)
#define MEM_FOOTER_ADDR(header)					((uint8_t *)header + sizeof(*header) + (header->size * sizeof(mem_word_type)))
#define MEM_WHOLE_SIZE_NODE(node)				(sizeof(mem_header) + sizeof(mem_word_type) * (node->size) + sizeof(mem_footer))
#define MEM_WHOLE_SIZE(size)					(sizeof(mem_header) + sizeof(mem_word_type) * (size) + sizeof(mem_footer))
#define MEM_HEADER_SIZE							(sizeof(mem_header))
#define MEM_PROVISIONING(needed_size)			((needed_size) * sizeof(mem_word_type) + sizeof(mem_footer))

static MEM_ALLOC_RET_TYPE mem_write_node(mem_header* dest, mem_header* src);


