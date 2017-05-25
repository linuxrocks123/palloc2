/*
 * palloc2_memory_controls.h
 *
 *  Created on: Apr 5, 2010
 *      Author: psimmons
 */

#ifndef PALLOC2_MEMORY_CONTROLS_H_
#define PALLOC2_MEMORY_CONTROLS_H_

#include <dlfcn.h>
#include <stdlib.h>
#include <strings.h>

#include <unistd.h>
#include <sys/syscall.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

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

#define C_AVOID_0 0x0000200000000000
#define C_AVOID_1 0x0000400000000000

//We need to override mmap and munmap to lock them against races

static plocklib_simple_t mmap_lock;

#if 0
static void* (*real_mmap)(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
     if(!real_mmap)
          real_mmap = dlsym(((void*) -1l),"mmap");

     plocklib_acquire_simple_lock(&mmap_lock);
     void* retval = real_mmap(addr,length,prot,flags,fd,offset);
     plocklib_release_simple_lock(&mmap_lock);
     return retval;
}

static int (*real_munmap)(void* addr, size_t length);
int munmap(void *addr, size_t length)
{
     if(!real_munmap)
          real_munmap = dlsym(((void*) -1l),"munmap");
     
     plocklib_acquire_simple_lock(&mmap_lock);
     int retval = real_munmap(addr,length);
     plocklib_release_simple_lock(&mmap_lock);
     return retval;
}
#endif

//Returns 1 if there is another line in /proc/self/maps, 0 otherwise
static char sbuffer[100];
static int sbuf_pos;
static int next_mapping(int fd, char* rbuf)
{
     int read_chars;
     int garbage_mode = 0;
     int rbuf_pos = 0;

     while(1)
     {
          if(!sbuf_pos)
          {
               read_chars = read(fd,sbuffer,100);
               if(!read_chars)
                    return rbuf_pos!=0;
               else if(read_chars<100)
                    sbuffer[read_chars]='\0';
          }

          while(sbuf_pos<100 && sbuffer[sbuf_pos])
               if(garbage_mode)
               {
                    if(sbuffer[sbuf_pos++]=='\n')
                    {
                         garbage_mode = 0;
                         if(rbuf_pos)
                              return 1;
                    }
               }
               else
               {
                    rbuf[rbuf_pos++] = sbuffer[sbuf_pos];
                    if(sbuffer[sbuf_pos++]==' ')
                         garbage_mode = 1;
               }

          sbuf_pos = 0;
     }

     return 1;
}

/*Open and parse /proc/self/maps to find out if a memory range is free.
  Oh my Cthulhu is this ugly.

  begin is inclusive and end exclusive.
  Must only be called with mmap_lock held.*/
static int is_memory_range_free(size_t begin, size_t end)
{
     int fd = open("/proc/self/maps",O_RDONLY);
     char buffer[100];
     size_t range_begin;
     size_t range_end;
     int retval = 1;

     while(next_mapping(fd,buffer))
     {
          char ibuf[50];
          int i=0, j=0;
          int length = 0;

          bzero(ibuf,50);
          while(1)
               if(buffer[i]=='-')
                    break;
               else
                    ibuf[j++] = buffer[i++];

          range_begin = strtoul(ibuf,NULL,16);
          j=0;
          i++;

          bzero(ibuf,50);
          while(1)
               if(buffer[i]==' ')
                    break;
               else
                    ibuf[j++] = buffer[i++];
          range_end = strtoul(ibuf,NULL,16);

          if(end <= range_begin)
          {
               retval = 1;
               break;
          }
          else if(range_end <= begin)
               continue;
          else
          {
               retval = 0;
               break;
          }
     }

     close(fd);

     bzero(sbuffer,100);
     sbuf_pos = 0;

     return retval;
}

static void* mmap_address_class(uint64_t address_class, uint64_t effective_address_class)
{
     dbgprintf("mmap_address_class: %zd, effective address class %zd\n",address_class,effective_address_class);
     static size_t next_attempt_for_class[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
     
     void* to_return = (void*)(-1);
     size_t range_end = (uint64_t)(-1);
     
     plocklib_acquire_simple_lock(&mmap_lock);

     do
     {
          if(!next_attempt_for_class[address_class])
               next_attempt_for_class[address_class] = (address_class > 15 ? C_AVOID_0 : C_AVOID_1) | (address_class << 41);
          
          to_return = next_attempt_for_class[address_class];
          next_attempt_for_class[address_class] = range_end = (size_t)to_return + (MIN_SUPERPAGE_SIZE << address_class);
     } while(!is_memory_range_free((size_t)to_return,range_end));

     if(!mmap(to_return,MIN_SUPERPAGE_SIZE << effective_address_class,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0))
     {
          dbgprintf("palloc: mmap_address_class failed\n");
          abort();
     }

     
     plocklib_release_simple_lock(&mmap_lock);

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
