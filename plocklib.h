#ifndef PLOCKLIB_H
#define PLOCKLIB_H

/*A synchronization library for palloc.
  See http://gcc.gnu.org/onlinedocs/gcc-4.1.0/gcc/Atomic-Builtins.html
*/     

typedef uint8_t plocklib_simple_t;

static inline void plocklib_simple_init(plocklib_simple_t* lock)
{
}

static inline void plocklib_simple_destroy(plocklib_simple_t* lock)
{
}

#ifdef __SUNPRO_C

#include <sys/atomic.h>

static inline int16_t plocklib_increment_and_fetch(uint16_t* to_increment)
{
	return atomic_inc_16_nv(to_increment);
}

static inline uint64_t plocklib_fetch_and_ffffffffffffffff(uint64_t* to_clear)
{
	asm("membar #StoreLoad");
	return atomic_swap_64(to_clear, (uint64_t)(-1));
}

static inline void plocklib_atomic_add(uint16_t* to_add, int add_amount)
{
	atomic_add_16(to_add,add_amount);
}

static inline void plocklib_atomic_and(uint64_t* to_and, uint64_t mask)
{
	atomic_and_64(to_and,mask);
}

static inline int plocklib_cas64(uint64_t* target, uint64_t oldval, uint64_t newval)
{
	if(atomic_cas_64(target,oldval,newval)!=oldval)
		return 0;
	else
		return 1;
}

static inline int plocklib_cas16(uint16_t* target, uint16_t oldval, uint16_t newval)
{
	if(atomic_cas_16(target,oldval,newval)!=oldval)
		return 0;
	else
		return 1;
}

static inline void plocklib_storestore_membar()
{
	/*asm("membar #StoreStore");*/
	membar_producer(); /*just in case doing it this way helps the Sun compiler*/
}

static inline void plocklib_acquire_simple_lock(plocklib_simple_t* lock)
{
     while(atomic_cas_uint((unsigned int*)lock,0,1));
     membar_enter();
}

static inline void plocklib_release_simple_lock(plocklib_simple_t* lock)
{
     unsigned int x = atomic_cas_8((uint8_t*)lock,1,0);
     assert(x);
     membar_exit();
}

struct plocklib_rw_lock
{
     volatile int readers;
     volatile int writer;
};

static inline void plocklib_become_reader(struct plocklib_rw_lock* lock)
{
     while(lock->writer);
     atomic_add_int((unsigned int*)(&(lock->readers)),1);
}

static inline void plocklib_resign_as_reader(struct plocklib_rw_lock* lock)
{
     atomic_add_int((unsigned int*)(&(lock->readers)),-1);
}

static inline int plocklib_request_writer_promotion(struct plocklib_rw_lock* lock)
{
     plocklib_resign_as_reader(lock);
     if(atomic_cas_uint((unsigned int*)(&(lock->writer)),0,1))
	  return 0;

     while(lock->readers);
     membar_enter();

     return 1;
}

static inline void plocklib_resign_as_writer(struct plocklib_rw_lock* lock)
{
     int x = atomic_cas_uint((unsigned int*)(&(lock->writer)),1,0);
     assert(x);
     membar_exit();
}

#else
static inline int16_t plocklib_increment_and_fetch(int16_t* to_increment)
{
	return __sync_add_and_fetch(to_increment,1);
}

static inline uint64_t plocklib_fetch_and_add(uint64_t* to_add, int delta)
{
	return __sync_fetch_and_add(to_add,delta);
}

static inline uint64_t plocklib_fetch_and_ffffffffffffffff(uint64_t* to_zero)
{
	/*this instruction is an implied full memory barrier*/
	return __sync_fetch_and_or(to_zero,-1);
}

static inline void plocklib_atomic_add(uint16_t* to_add, int delta)
{
	__sync_fetch_and_add(to_add,delta);
}

static inline void plocklib_atomic_and(uint64_t* to_and, uint64_t mask)
{
	__sync_fetch_and_and(to_and,mask);
}

static inline int plocklib_cas64(uint64_t* target, uint64_t oldval, uint64_t newval)
{
	return __sync_bool_compare_and_swap(target,oldval,newval);
}

static inline int plocklib_cas16(uint16_t* target, uint16_t oldval, uint16_t newval)
{
	return __sync_bool_compare_and_swap(target,oldval,newval);
}

static inline void plocklib_storestore_membar()
{
	/*this function is not used in places where it would be necessary on x86-64*/
}

static inline void plocklib_acquire_simple_lock(plocklib_simple_t* lock)
{
     while(__sync_lock_test_and_set(lock,1));
}

static inline void plocklib_release_simple_lock(plocklib_simple_t* lock)
{
     __sync_lock_release(lock);
}

struct plocklib_rw_lock
{
     volatile int readers;
     volatile int writer;
};

static inline void plocklib_become_reader(struct plocklib_rw_lock* lock)
{
     while(lock->writer);
     __sync_fetch_and_add(&(lock->readers),1);
}

static inline void plocklib_resign_as_reader(struct plocklib_rw_lock* lock)
{
     __sync_fetch_and_sub(&(lock->readers),1);
}

/*Upon grant of writership, 1 is returned.  0 returned on denial.
  If request is denied, the requester MUST UNDERSTAND
  that he no longer has even reader access to the locked data.
*/
static inline int plocklib_request_writer_promotion(struct plocklib_rw_lock* lock)
{
     plocklib_resign_as_reader(lock);
     if(__sync_lock_test_and_set(&(lock->writer),1))
          return 0;

     /*We now have the writer lock.
       Wait for remaining readers to relinquish their locks.
     */
     while(lock->readers);

     return 1;
}

/*Resign ownership of writer lock.*/
static inline void plocklib_resign_as_writer(struct plocklib_rw_lock* lock)
{
     __sync_lock_release(&(lock->writer));
}
#endif

static inline void plocklib_rw_init(struct plocklib_rw_lock* lock)
{
}

#endif
