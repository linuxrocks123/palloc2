#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#ifndef NDEBUG
#define DEBUG
#endif
#ifdef DEBUG
#include <stdio.h>
#define dbgprintf(...) fprintf(stderr,__VA_ARGS__)
#else
#define dbgprintf(...) ;
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "palloc_config.h"
#include "plocklib.h"

struct page_record;

/*This structure is designed to be cachelicious.
   Please take care to preserve this property if you modify it.*/
struct thread_record
{
	struct page_record* buckets[NUM_PALLOC_BUCKETS*2];

	plocklib_simple_t threadlock;
	uint16_t pad16;
	plocklib_simple_t pad8;
    	uint32_t pad32;
    	uint64_t pad64;
};

/*Also cachelicious.*/
struct page_record
{
    uint16_t prefilled_entries; /*high bit 1 indicates remote frees pending*/
    uint16_t cached_predecessor_entries;
    uint16_t free_entries;
    uint16_t owning_thread; /*could in principle calculate from chain_head_ptr*/

    uint64_t bitmap[PALLOC_BITVEC_ENTRIES];

    struct page_record** chain_head_ptr;
    struct page_record*  chain_back_ptr;
    struct page_record*  chain_forward_ptr;

    uint64_t superpage_size; /*Size of the superpage for freeing.  DO NOT calculate based on the offset of chain_head_ptr from the first size class*/
    uint64_t* remote_free_array; /*one cache line worth of data -- parallels bitmap*/
    int16_t pending_remote_frees;
    uint16_t pad16;
    uint32_t pad32;
    uint64_t pad64;
};

/*Huge static array that goes in the BSS.  This way, we don't need expensive initialization in the library constructor.*/
static struct thread_record threads[PALLOC_MAX_THREADS];

/*Per-thread index into the threads array*/
static __thread uint16_t tls_index = 0;

#include "palloc2_memory_controls.h"
#include "threadindexlib.h"

static inline size_t align_size_class(size_t orig_size, int* size_class)
{
	if(!orig_size)
		orig_size++;
	int first_set_bit = fls64(orig_size);
	size_t new_size = 1L << first_set_bit;

	if(new_size!=orig_size)
	{
		new_size<<=1;
		first_set_bit++;
	}
	if(new_size < MIN_SIZE_CLASS)
	{
		new_size = MIN_SIZE_CLASS;
		first_set_bit = MIN_SET_BIT;
	}

	*size_class = max(0,first_set_bit - MIN_SET_BIT);
	return new_size;
}

static inline void process_remote_frees(struct page_record* bucket)
{
	dbgprintf("processing remote frees tls_index %d\n",tls_index);
	if(!bucket->remote_free_array)
		return;

	int remote_frees_performed = 0;
	int i;
	for(i=0; i<PALLOC_BITVEC_ENTRIES; i++)
	{
		uint64_t ready_frees = plocklib_fetch_and_ffffffffffffffff(bucket->remote_free_array + i);
		bucket->bitmap[i]&=ready_frees;
		remote_frees_performed+=popcount(~ready_frees);
	}

	bucket->free_entries+=remote_frees_performed;
	plocklib_atomic_add((uint16_t*)(&bucket->pending_remote_frees),-remote_frees_performed);
}

static inline void* heapspace(struct page_record** bucket, int size_class)
{
	dbgprintf("heapspace: size class %d\n",size_class);
	if(!*bucket)
	{
		dbgprintf("heapspace: no bucket\n");
		*bucket = (struct page_record*)(mmap_address_class(size_class,min(size_class,PALLOC_HACK_MAX_SIZE_CLASS)));
		int prefilled_entries = ALIGN_SIZE(sizeof(struct page_record),MIN_SIZE_CLASS << size_class)/(MIN_SIZE_CLASS << size_class);
		int64_t first_bitmap_entry = (int64_t)(0x8000000000000000L) /*make sure this is computed on the fly*/ >> (prefilled_entries - 1);
		(*bucket)->prefilled_entries = prefilled_entries;
		(*bucket)->cached_predecessor_entries = (uint16_t)(-1);
		(*bucket)->free_entries = PALLOC_PAGE_ENTRIES - prefilled_entries;
		(*bucket)->owning_thread = tls_index;
		(*bucket)->bitmap[0] = first_bitmap_entry;
		(*bucket)->chain_head_ptr = bucket;
		*(bucket + NUM_PALLOC_BUCKETS) = *bucket;
		(*bucket)->superpage_size = MIN_SUPERPAGE_SIZE << min(size_class,PALLOC_HACK_MAX_SIZE_CLASS);

		if(size_class > PALLOC_HACK_MAX_SIZE_CLASS)
		{
			int i = PALLOC_BITVEC_ENTRIES >> (size_class - PALLOC_HACK_MAX_SIZE_CLASS);
			if(!i)
                                abort();
                        
                        while(i < PALLOC_BITVEC_ENTRIES)
			{
				(*bucket)->prefilled_entries+=bits_in(uint64_t);
				(*bucket)->bitmap[i] = (uint64_t)(-1);
                                i++;
			}
		}
	}
	(*bucket)->free_entries--;

	assert(!(*bucket)->chain_back_ptr);
	dbgprintf("heapspace: bucket valid\n");

	/*Update remote free buffer*/
	if(!(*bucket)->free_entries)
		process_remote_frees(*bucket);

	int i=0;
	while((*bucket)->bitmap[i]==(uint64_t)(-1))
		i++;

	dbgprintf("heapspace: found partially free bitmap entry 0x%zx, index %d\n",(*bucket)->bitmap[i],i);

	int bitpos = flz64((*bucket)->bitmap[i]);
	dbgprintf("heapspace: found free bit offset %d\n",bitpos);
	(*bucket)->bitmap[i] |= 1L << bitpos;

	/*Save the base address of the superpage for the chunk to be returned by this allocation.*/
	uint8_t* page_base = (uint8_t*)*bucket;

	dbgprintf("heapspace: chkpt 1\n");

	/*If we've filled the head, make the next entry the head of the list*/
	if(!(*bucket)->free_entries)
	{
		dbgprintf("heapspace: filled page\n");
		struct page_record* next_chain_ptr = (*bucket)->chain_forward_ptr;
		(*bucket)->chain_back_ptr = NULL;
                (*bucket)->cached_predecessor_entries = (uint16_t)(-1);
		(*bucket)->chain_forward_ptr = NULL;
		assert(!(*bucket)->chain_back_ptr);
		*bucket = next_chain_ptr;
		if(*bucket)
		{
			assert((*bucket)->chain_back_ptr);
			(*bucket)->chain_back_ptr = NULL;
                        (*bucket)->cached_predecessor_entries = (uint16_t)(-1);
		}
                else
                        *(bucket + NUM_PALLOC_BUCKETS) = NULL;
		dbgprintf("heapspace: handled free page\n");
	}

	return page_base + (i*bits_in(uint64_t) + (bits_in(uint64_t) - 1 - bitpos))*(MIN_SIZE_CLASS << size_class);
}

void* malloc(size_t size)
{
	dbgprintf("allocating: %zd from thread %d...\n",size,tls_index);
    int size_class;
    size = align_size_class(size,&size_class);

    void* to_return;
    if(size_class >= PALLOC_HACK_ABSURDLY_HUGE_SIZE_CLASS)
    	to_return = mmap_address_class(size_class,size_class - PALLOC_HACK_SINGLETON_MMAP_OFFSET);
    else
    	to_return = heapspace(threads[tls_index].buckets + size_class, size_class);
    dbgprintf("...0x%zx thread %d\n",to_return,tls_index);
    return to_return;
}

static inline void local_free(void* address, struct page_record* record, int size_class, int bitmap_index, uint64_t free_mask)
{
	dbgprintf("local_free: 0x%zx\n",address);
	record->bitmap[bitmap_index]&=free_mask;
	uint16_t old_free_entries = record->free_entries;
	record->free_entries++;

#if 0
	if(record->cached_predecessor_entries!=(uint16_t)(-1) && record->free_entries == PALLOC_PAGE_ENTRIES - record->prefilled_entries) /*we are totally free*/
	{
		dbgprintf("local_free: page deallocation\n");
		/*We need to figure out whether to keep ourselves as a buffer*/
		int keep = 1;
		if(record->chain_forward_ptr) /*we have a successor, which may also be close to free*/
			keep = 0;
		else if(record->chain_back_ptr->free_entries >= PALLOC_PAGE_ENTRIES / 2)
			keep = 0;
		if(!keep) /*then free ourselves*/
		{
			record->chain_back_ptr->chain_forward_ptr = record->chain_forward_ptr;
			if(record->chain_forward_ptr)
				record->chain_forward_ptr->chain_back_ptr = record->chain_back_ptr;
                        else
                                *(record->chain_head_ptr + NUM_PALLOC_BUCKETS) = record->chain_back_ptr;

			munmap(record,record->superpage_size);
		}
	}
	else
#endif
        if(!old_free_entries) /*we were previously full and need to insert ourselves as tail of our chain*/
	{
		dbgprintf("page addition to list\n");
		if(*(record->chain_head_ptr + NUM_PALLOC_BUCKETS))
		{
			record->chain_back_ptr = *(record->chain_head_ptr + NUM_PALLOC_BUCKETS);
                        record->cached_predecessor_entries = record->chain_back_ptr->free_entries;
                        (*(record->chain_head_ptr + NUM_PALLOC_BUCKETS))->chain_forward_ptr = record;
                        *(record->chain_head_ptr + NUM_PALLOC_BUCKETS) = record;
                }
                else
                {
                        *(record->chain_head_ptr) = record;
                        *(record->chain_head_ptr + NUM_PALLOC_BUCKETS) = record;
                        record->cached_predecessor_entries = (uint16_t)(-1);
                }
	}
	else if(record->cached_predecessor_entries!=(uint16_t)(-1) && record->free_entries > record->cached_predecessor_entries) /*we need to check if we should swap ourselves down the list -- use cached_predecessor_entries==-1 to enforce never swapping if we are head or next-to-head*/
	{
		dbgprintf("page list restructuring\n");
	    record->cached_predecessor_entries = record->chain_back_ptr==*(record->chain_head_ptr) ? (uint16_t)(-1) : record->chain_back_ptr->free_entries;
	    if(record->free_entries > record->cached_predecessor_entries)
	    {
	    	struct page_record* successor = record->chain_forward_ptr;
	    	struct page_record* predecessor = record->chain_back_ptr;
	    	record->chain_back_ptr = predecessor->chain_back_ptr;
    		record->cached_predecessor_entries = record->chain_back_ptr==*(record->chain_head_ptr) ? (uint16_t)(-1) : record->chain_back_ptr->free_entries;
    		record->chain_back_ptr->chain_forward_ptr = record;
	    	record->chain_forward_ptr = predecessor;
	    	predecessor->chain_back_ptr = record;
	    	predecessor->cached_predecessor_entries = record->free_entries;
	    	predecessor->chain_forward_ptr = successor;
	    	if(!successor)
	    		*(predecessor->chain_head_ptr + NUM_PALLOC_BUCKETS) = predecessor;
	    	else /*We are intentionally being conservative and NOT updating predecessor->cached_successor_free_entries in order to avoid a cache miss.*/
	    		successor->chain_back_ptr = predecessor;
	    }
	}
	dbgprintf("local_free end chkpt\n");
}

static inline void remote_free(void* address, struct page_record* record, int size_class, int bitmap_index, uint64_t free_mask)
{
#if 0
	//See if we can steal it
	uint16_t remote_owner = record->owning_thread;
	if(remote_owner > PALLOC_MAX_THREADS && remote_owner - PALLOC_MAX_THREADS != tls_index)
		if(plocklib_cas16(&record->owning_thread, remote_owner, tls_index))
		{
			int size_class;
			align_size_class(record->superpage_size / PALLOC_PAGE_ENTRIES,&size_class);
			record->chain_head_ptr = threads[tls_index].buckets + size_class;
			local_free(address,record,size_class,bitmap_index,free_mask);
			return;
		}
#endif

	dbgprintf("remote_free tls_index: %d\n",tls_index);
	if(!record->remote_free_array)
	{
		dbgprintf("remote_free: create array\n");
		/*This is trickier than it might seem as we need to ensure we do not step on another thread's toes.*/
		uint64_t* potential_free_array = get_rfree_buffer();
		dbgprintf("remote_free: potential_free_array: 0x%zx\n",potential_free_array);
		if(!plocklib_cas64((uint64_t*)(&record->remote_free_array),NULL,(uint64_t)(potential_free_array)))
			release_rfree_buffer(potential_free_array);
	}

	dbgprintf("remote_free: chkpt 1\n");

	/*Perform the actual free.*/
	uint16_t prefilled_entries = record->prefilled_entries;
	plocklib_atomic_and(record->remote_free_array + bitmap_index,free_mask);
	if(plocklib_increment_and_fetch(&record->pending_remote_frees) + prefilled_entries == PALLOC_PAGE_ENTRIES)
		munmap(record,record->superpage_size);
}

void free(void* address)
{
	dbgprintf("free: 0x%zx thread %d\n",address,tls_index);
	if(!address)
		return;
	int size_class = get_size_class_from_address((size_t)address);
	if(size_class >= PALLOC_HACK_ABSURDLY_HUGE_SIZE_CLASS)
	{
		munmap(address,MIN_SIZE_CLASS << size_class);
		return;
	}
	dbgprintf("free: size_class: %d\n",size_class);
	struct page_record* address_page_record = (struct page_record*)((size_t)address & ~((MIN_SUPERPAGE_SIZE << size_class) - 1));
	dbgprintf("free: address_page_record: 0x%zx\n",address_page_record);
	size_t byte_offset = (size_t)address - (size_t)address_page_record;
	dbgprintf("free: byte_offset: %zd\n",byte_offset);
	int chunk_offset = byte_offset >> (MIN_SET_BIT + size_class);
	dbgprintf("free: chunk_offset: %d\n",chunk_offset);
	int bitmap_index = chunk_offset/bits_in(uint64_t);
	dbgprintf("free: bitmap_index: %d\n",bitmap_index);
	int bitmap_offset = chunk_offset%bits_in(uint64_t);
	dbgprintf("free: bitmap_offset: %d\n",bitmap_offset);
	uint64_t free_mask = 0x8000000000000000L; /*Make sure the compiler computes this on the fly.  It should if it's not retarded.*/
	free_mask>>=bitmap_offset;
	free_mask=~free_mask;
	dbgprintf("free: free_mask: 0x%zx\n",free_mask);

	if(address_page_record->owning_thread==tls_index)
	    local_free(address,address_page_record,size_class,bitmap_index,free_mask);
	else
	    remote_free(address,address_page_record,size_class,bitmap_index,free_mask);
}

void __attribute__ ((constructor)) palloc_initialize()
{
	plocklib_simple_init(&global_rfree_lock);
	plocklib_simple_init(&id_lock);
	plocklib_acquire_simple_lock(&threads[0].threadlock);
}

void *realloc(void *ptr, size_t size)
{
	dbgprintf("realloc: 0x%zx %zd",ptr,size);
	int size_class, old_size_class;
	align_size_class(size,&size_class);
    if(ptr==NULL)
    	return malloc(size);
    else if(size==0)
    {
    	free(ptr);
    	return NULL;
    }
    else if((old_size_class = get_size_class_from_address((size_t)ptr)) >= size_class)
    	return ptr;
    else
    {
    	void* to_return = malloc(size);
    	memcpy(to_return,ptr,MIN_SIZE_CLASS << old_size_class);
    	free(ptr);
    	return to_return;
    }
}

void *calloc(size_t nelem, size_t elsize)
{
	dbgprintf("calloc: %zd %zd\n",nelem,elsize);
    size_t size = nelem * elsize;
    void* ptr = malloc(size);
    if(ptr != NULL)
    	memset(ptr,0,size);
    return ptr;
}

void * memalign (size_t alignment, size_t size);

int posix_memalign (void **memptr, size_t alignment, size_t size)
{
  dbgprintf("posix_memalign: 0x%zx %zd %zd\n",memptr,alignment,size);
  // Check for non power-of-two alignment.
  if ((alignment == 0) ||
      (alignment & (alignment - 1)))
    {
      return EINVAL;
    }
  void * ptr = memalign (alignment, size);
  if (!ptr) {
    return ENOMEM;
  } else {
    *memptr = ptr;
    return 0;
  }
}

void * memalign (size_t alignment, size_t size)
{
  dbgprintf("memalign: %zd %zd\n",alignment,size);
  // NOTE: This function is deprecated.
  if (alignment > size)
    return malloc (alignment);
  else
    return malloc (size);
}

void* valloc(size_t size)
{
   dbgprintf("valloc: %zd\n",size);
   return memalign(sysconf(_SC_PAGESIZE),size);
}
