#include <stdio.h>
#include "mem_op.h"
#include "mem_op_natvis.h"
#include <stdlib.h>

#define MEM_HEAD	(&mem_heap[0])

#ifdef MEM_WATERMARKS
const uint32_t mem_header_start = 0x11111111;
const uint32_t mem_header_end = 0x11111111;
const uint32_t mem_footer_start = 0x22222222;
const uint32_t mem_footer_end = 0x22222222;
#endif

int debug_flag = 0;

mem_word_type mem_heap[MEM_HEAP_SIZE] = { 0 };
mem_word_type mem_heap_big_endian[MEM_HEAP_SIZE] = { 0 };

mem_header * mem_head;
mem_header * mem_available;
mem_header * first_alloc_node_header;
mem_header * mem_lambda_header;
mem_footer * mem_lambda_footer;
mem_footer * first_alloc_node_footer;

static inline MEM_ALLOC_RET_TYPE mem_write_footer(mem_header * dest)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	mem_footer * footer = MEM_FOOTER_ADDR(dest);
#ifdef MEM_WATERMARKS
	footer->mem_footer_start = mem_footer_start;
	footer->mem_footer_end = mem_footer_end;
#endif
	return ret_val;
}
static inline MEM_ALLOC_RET_TYPE mem_write_header(mem_header* mem_header_dest, mem_header* mem_header_src)
{
	MEM_ALLOC_RET_TYPE ret_val = MEM_ALLOC_RET_TYPE_OK;
	*mem_header_dest = *mem_header_src;
#ifdef MEM_WATERMARKS
	mem_header_dest->mem_header_start = mem_header_start;
	mem_header_dest->mem_header_end = mem_header_end;
#endif
	return ret_val;
}
static inline MEM_ALLOC_RET_TYPE mem_write_node(mem_header * dest_node, mem_header * src_node)
{
	MEM_ALLOC_RET_TYPE ret_val = MEM_ALLOC_RET_TYPE_OK;
	if (1 == debug_flag)
	{
		volatile int x = 0;
		x = x;

		ret_val = mem_write_header(dest_node, src_node);
		first_alloc_node_header = dest_node;
		if (ret_val != MEM_ALLOC_RET_TYPE_OK)
		{
			return ret_val;
		}
		ret_val = mem_write_footer(dest_node);
		first_alloc_node_footer = MEM_FOOTER_ADDR(dest_node);
		

	}
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_write_node_with_next(mem_header * dest_node, mem_header * src_node, mem_header * next_node)
{
	MEM_ALLOC_RET_TYPE ret_val = MEM_ALLOC_RET_TYPE_OK;
	ret_val = mem_write_header(dest_node, src_node);
	if (ret_val != MEM_ALLOC_RET_TYPE_OK)
	{
		return ret_val;
	}
	if ((uint8_t*)(next_node) < ((uint8_t*)dest_node + sizeof(mem_header) + sizeof(mem_footer)))
	{
		return MEM_ALLOC_RET_TYPE_NOK;
	}
	dest_node->size = (uint8_t*)(next_node) - (uint8_t*)dest_node - sizeof(mem_header) - sizeof(mem_footer);
	ret_val = mem_write_footer(dest_node);
	return ret_val;
}
static inline MEM_ALLOC_RET_TYPE mem_fill(mem_header* dest)
{
	for (uint32_t i = 0; i < dest->size; i++)
	{
		dest->start[i] = 0xCD;
	}
	return (MEM_ALLOC_RET_TYPE)(0);
}


static inline reverse_endian(void* dest, void* src, int32_t sz)
{
	for (int32_t i = sz - 1; i >= 0; i--)
	{
		((uint8_t*)dest)[i - sz] = ((uint8_t*)(src))[i];
	}
}

void mem_init(void)
{

	mem_lambda_header = (mem_header*)((uint8_t*)(&mem_heap[MEM_HEAP_SIZE - 1]) - MEM_WHOLE_SIZE(0));
	mem_lambda_footer = (mem_footer*)MEM_FOOTER_ADDR(mem_lambda_header);
	mem_lambda_header->avail = 0;
	mem_lambda_header->next = NULL;
	mem_lambda_header->size = 0;
	mem_write_node(mem_lambda_header, mem_lambda_header);
	//(*(uint64_t*)(&mem_lambda_header->start[0])) = 0xDEADBEEF;

	mem_head = ((mem_header*)&mem_heap[0]);
	mem_head->avail = 1;
	mem_head->next = mem_lambda_header;
	mem_write_node_with_next((uint8_t*)(&mem_heap[0]), (uint8_t*)mem_head, mem_lambda_header);
}

void* mem_alloc(mem_size N)
{
	mem_word_type * ret_val = NULL;
	mem_header * p = MEM_HEAD;
	mem_footer * p_footer = NULL;
	const mem_word_type mem_provisioning = MEM_WHOLE_SIZE(N);

	while (p != mem_lambda_header)
	{
		if (MEM_AVAIL(p) == 0)
		{
			p = MEM_LINK(p);
			continue;
		}

		if (MEM_SIZE(p) < mem_provisioning)
		{
			p = MEM_LINK(p);
			continue;
		}
		if (1 == 1)
		{
			volatile int x = 0;
			x = x;
		}
		mem_size k = MEM_SIZE(p) - mem_provisioning;
		mem_header temp_src;
		temp_src.avail = 1;
		temp_src.next = MEM_LINK(p);
		temp_src.size = k;


		debug_flag = 1;
		if	(
			(MEM_ALLOC_RET_TYPE_OK == mem_write_node(p + mem_provisioning, &temp_src))
			)
		{
			MEM_SIZE(p) = N;
			MEM_LINK(temp) = MEM_LINK(p);
			MEM_LINK(p) = temp;
			MEM_SIZE(temp) = k;
			ret_val = (uint8_t*)(&(p->start[0]));
			break;
		}
	}
	return (void*)(ret_val);
}

void* mem_free(void* ptr)
{
	return NULL;
}

int main()
{
	char c;
	mem_init();
	mem_header* dummy;
	uint8_t* ptr = (uint8_t*)mem_alloc(20);
	uint8_t* ptr_header = ptr - sizeof(*dummy);
	printf("AICI %d\n", sizeof(dummy->avail));
	printf("%d\n", sizeof(dummy->next));
	printf("%d\n", sizeof(dummy->size));
	printf("%d\n", sizeof(&dummy->start[0]));
	mem_fill(ptr_header);
	printf("%p", mem_available);
	scanf_s("%c", &c);
	return 0;
}