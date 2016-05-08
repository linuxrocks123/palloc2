/*
 * palloc2_memory_controls.h
 *
 *  Created on: Apr 5, 2010
 *      Author: psimmons
 */

#ifndef PALLOC2_MEMORY_CONTROLS_H_
#define PALLOC2_MEMORY_CONTROLS_H_

/*We need to steal five bits of the address to use for class identification.
 * This is our approach for accomplishing that:
 * - Use the top two bits of the address space for conflict avoidance control.
 * - Use the next 5 bits for class identification.
 * - Set the next bit to "1" so that we start allocating in approximately the middle of the size class.
 * - Next time we call mmap for this size class, request one superpage below or above the last returned address
 *    (depending on whether the last returned address was above or below what we were returned).
 */

static inline int check_mmap_address(size_t address_to_check, int address_class)
{
	size_t to_return = address_to_check;
	to_return &= ~(3L << 46);
	to_return >>= 41;

	return to_return==address_class && address_to_check%(MIN_SUPERPAGE_SIZE << address_class)==0;
}

static inline int get_size_class_from_address(size_t to_return)
{
	to_return &= ~(3L << 46);
	to_return >>= 41;

	return to_return;
}

static void* mmap_address_class(uint64_t address_class, uint64_t effective_address_class)
{
	dbgprintf("mmap_address_class: %zd, effective address class %zd\n",address_class,effective_address_class);
	static plocklib_simple_t mmap_address_class_lock;
	static int8_t conflict_avoidance[] = {3,3,3,3,3,3,3,3,3,3,3,3,3, /*0b11*/
			                              3,3,3,3,3,3,3,3,3,3,3,3,3,
										  3,3,3,3,3,3};
	static size_t next_attempt_for_class[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

	void* to_return = (void*)(-1);

	plocklib_acquire_simple_lock(&mmap_address_class_lock);

	if(!next_attempt_for_class[address_class])
		next_attempt_for_class[address_class] = (uint64_t)conflict_avoidance[address_class] << 46 | address_class << 41 | 1L << 40;

	to_return = mmap((void*)next_attempt_for_class[address_class],MIN_SUPERPAGE_SIZE << effective_address_class,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	while(!check_mmap_address((size_t)to_return,address_class))
	{
		if(to_return!=(void*)-1)
			munmap(to_return,MIN_SUPERPAGE_SIZE << effective_address_class);
		conflict_avoidance[address_class]--;
		if(conflict_avoidance[address_class] < 0)
                {
                        dbgprintf("aborting in mmap_address_class\n");
			abort();
                }
		next_attempt_for_class[address_class] = (uint64_t)conflict_avoidance[address_class] << 46 | address_class << 41 | 1L << 40;
		to_return = mmap((void*)next_attempt_for_class[address_class],MIN_SUPERPAGE_SIZE << effective_address_class,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	}

	next_attempt_for_class[address_class] = (size_t)to_return + (((size_t)to_return <= next_attempt_for_class[address_class] ? -MIN_SUPERPAGE_SIZE : MIN_SUPERPAGE_SIZE) << address_class);

	plocklib_release_simple_lock(&mmap_address_class_lock);

    return to_return;
}

static plocklib_simple_t global_rfree_lock;
static uint64_t* next_rfree_buffer;

static uint64_t* get_rfree_buffer()
{
	dbgprintf("get_rfree_buffer\n");
    uint64_t* to_return;

    plocklib_acquire_simple_lock(&global_rfree_lock);
    dbgprintf("get_rfree_buffer chkpt 1\n");
    assert(next_rfree_buffer!=(uint64_t*)(-1));
    if(next_rfree_buffer==NULL)
    {
    	dbgprintf("NULL rfree buffer\n");
    	next_rfree_buffer = (uint64_t*)(mmap(NULL,MIN_SUPERPAGE_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    	assert(next_rfree_buffer!=(uint64_t*)(-1));

    	uint64_t* i;
    	for(i = next_rfree_buffer; i+2*PALLOC_BITVEC_ENTRIES <= (uint64_t*)((size_t)next_rfree_buffer + MIN_SUPERPAGE_SIZE); i+=PALLOC_BITVEC_ENTRIES)
    		*i = (size_t)(i+PALLOC_BITVEC_ENTRIES);
    }

    to_return = next_rfree_buffer;
    dbgprintf("get_rfree_buffer: to_return: 0x%zx\n",to_return);
    next_rfree_buffer = (uint64_t*)(*to_return);
    dbgprintf("get_rfree_buffer chkpt 2\n");

    plocklib_release_simple_lock(&global_rfree_lock);

    dbgprintf("get_rfree_buffer chkpt 3\n");
    memset(to_return,-1,sizeof(uint64_t)*PALLOC_BITVEC_ENTRIES);
    return to_return;
}

static void release_rfree_buffer(uint64_t* to_free)
{
    plocklib_acquire_simple_lock(&global_rfree_lock);
    *to_free = (size_t)next_rfree_buffer;
    next_rfree_buffer = to_free;
    assert(next_rfree_buffer!=(uint64_t*)(-1));
    plocklib_release_simple_lock(&global_rfree_lock);
}

#endif /* PALLOC2_MEMORY_CONTROLS_H_ */
