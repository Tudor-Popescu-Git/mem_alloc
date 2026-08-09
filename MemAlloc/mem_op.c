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
mem_header * mem_header_watch;
mem_footer * mem_lambda_footer;
mem_footer * first_alloc_node_footer;
mem_footer * mem_footer_watch;
mem_bool     mem_check;

static inline MEM_ALLOC_RET_TYPE mem_payload_to_header(mem_header** header, void* payload)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	*header = (uint8_t*)payload - sizeof(mem_header);
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_coalesce_headers(mem_header** header1, mem_header * header2)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	mem_header* temp_header = *header1;
	if (!(MEM_AVAIL(*header1) == 1) || (*header1 == header2))
	{
		return ret_val;
	}
	MEM_LINK(temp_header) = MEM_LINK(header2);
	MEM_SIZE(temp_header) = MEM_SIZE(temp_header) + MEM_WHOLE_SIZE_NODE(header2);
	MEM_AVAIL(temp_header) = 1;
	//(*temp_header).next = MEM_LINK(header2);
	//(*temp_header).size = MEM_SIZE(temp_header) + MEM_WHOLE_SIZE_NODE(header2);
	//(*temp_header).avail = 1;
	mem_write_node(*header1, temp_header);
	//TODO Must update footer
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_header_to_prev_footer(mem_footer** footer, mem_header* header)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	*footer = (mem_footer*)((uint8_t*)header - sizeof(mem_header));
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_header_to_footer(mem_footer** footer, mem_header* header)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	*footer = (mem_footer *) ((uint8_t*)(header) + sizeof(mem_header) + header->size);
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_footer_to_prev_header(mem_header ** header, mem_footer* footer)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	*header = (mem_header *) ((uint8_t*)footer - footer->size - sizeof(mem_header));
	return ret_val;
}

__declspec(dllexport) MEM_ALLOC_RET_TYPE mem_footer_header_check(mem_bool * check, mem_footer * footer, mem_header * header)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	mem_footer * i_footer;
	mem_header * i_header;
	mem_header_to_footer(&i_footer, header);
	mem_footer_to_prev_header(&i_header, i_footer);
	*check = (header == i_header);
	return ret_val;
}

static inline MEM_ALLOC_RET_TYPE mem_write_footer(mem_header * dest)
{
	MEM_ALLOC_RET_TYPE ret_val = (MEM_ALLOC_RET_TYPE)(0);
	mem_footer * footer = MEM_FOOTER_ADDR(dest);
#ifdef MEM_WATERMARKS
	footer->mem_footer_start = mem_footer_start;
#endif
	footer->size = dest->size;
	MEM_AVAIL(footer) = MEM_AVAIL(dest);
#ifdef MEM_WATERMARKS
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

	ret_val = mem_write_header(dest_node, src_node);
	first_alloc_node_header = dest_node;
	if (ret_val != MEM_ALLOC_RET_TYPE_OK)
	{
		return ret_val;
	}
	ret_val = mem_write_footer(dest_node);
#ifdef MEM_CLEAR_PAYLOAD
	mem_fill(dest_node);
#endif
	first_alloc_node_footer = MEM_FOOTER_ADDR(dest_node);

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
		dest->start[i] = 0x00;
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
	mem_lambda_header->mem_header_start = mem_header_start;
	mem_lambda_header->mem_header_end = mem_header_end;
	mem_write_node(mem_lambda_header, mem_lambda_header);
	//(*(uint64_t*)(&mem_lambda_header->start[0])) = 0xDEADBEEF;

	mem_head = ((mem_header*)&mem_heap[0]);
	mem_head->avail = 1;
	mem_head->next = mem_lambda_header;
	mem_write_node_with_next((uint8_t*)(&mem_heap[0]), (uint8_t*)mem_head, mem_lambda_header);
	mem_footer_header_check(&mem_check, mem_lambda_footer, mem_lambda_header);
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

		mem_size k;
		mem_header* temp = NULL;
		int no_force = 1;
		if (MEM_SIZE(p) > mem_provisioning + MEM_WHOLE_SIZE(0) && no_force == 1)
		{
			// must create new node with size remaining MEM_SIZE(p) - k
			k = mem_provisioning;
			temp = (uint8_t*)(p)+k;
			MEM_AVAIL(temp) = 1;
			MEM_LINK(temp) = MEM_LINK(p);
			MEM_SIZE(temp) = MEM_SIZE(p) - k;

			debug_flag = 1;
			if	(
					(MEM_ALLOC_RET_TYPE_OK == mem_write_node(temp, temp))
				)
			{
				//if new node has been created, change size
				//(*p).size -= sizeof(mem_footer) + MEM_WHOLE_SIZE_NODE(temp);
				//(*p).size -=  MEM_WHOLE_SIZE_NODE(temp);
				(*p).size = N;
				//if new node has been created, make MEM_LINK(p) point to newly created node 
				(*p).next = temp;
				MEM_LINK(p) = temp;
			}
		}
		else
		{
			// if no other node can fit in the remaining space,
			// allocate whole space to current node
		}
		//TODO mem_alloc doesn't write the right nodes, current node footer and header of next node are too far apart
		MEM_AVAIL(p) = 0;
		mem_write_node(p, p);
		mem_header_to_footer(&p_footer, p);
		mem_footer_header_check(&mem_check, p_footer, p);
		ret_val = (uint8_t*)(&(p->start[0]));
		break;
	}
	return (void*)(ret_val);
}

void mem_free(void* ptr)
{
	mem_header* header;
	mem_header* prev_header;
	mem_footer* temp_footer;
	mem_header* temp_header;
	mem_payload_to_header(&header, ptr);
	prev_header = header;
	if ((mem_word_type*)header != (mem_word_type*)&mem_heap[0])
	{
		mem_header_to_prev_footer(&temp_footer, header);
		mem_footer_to_prev_header(&prev_header, temp_footer);
	}
	else
	{
		;
	}
	if (MEM_AVAIL(prev_header) == 1)
	{
		mem_coalesce_headers(&prev_header, header);
	}
	else
	{
		;
	}
	if (header != mem_lambda_header)
	{
		temp_header = header;
		if (MEM_AVAIL(MEM_LINK(header)) == 1)
		{
			MEM_AVAIL(header) = 1;
			mem_coalesce_headers(&temp_header, MEM_LINK(header));
		}
	}

	return;
}

int main()
{
	char c;
	mem_init();
	mem_header* dummy;
	uint8_t* ptr = (uint8_t*)mem_alloc(0x10);
	mem_fill(ptr);
	mem_free(ptr);
	uint8_t* ptr_header = ptr - sizeof(*dummy);
	//printf("AICI %zu\n", sizeof(dummy->avail));
	//printf("%zu\n", sizeof(dummy->next));
	//printf("%zu\n", sizeof(dummy->size));
	//printf("%zu\n", sizeof(&dummy->start[0]));
	mem_fill(ptr_header);
	printf("%p", mem_available);
	scanf_s("%c", &c);
	return 0;
}