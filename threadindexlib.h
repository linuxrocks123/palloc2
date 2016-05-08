#ifndef THREADINDEXLIB_H
#define THREADINDEXLIB_H

#ifndef SPECIALSNOWFLAKE
#include <dlfcn.h>
#else
#include <pthread.h>
#endif

static plocklib_simple_t id_lock;
static int next_thread_id = 1;

struct wrapper_struct
{
     void *(*start_routine) (void*);
     void* arg;
     int thread_id;
};

static void* wrapped_startthread(void* info_)
{
     struct wrapper_struct* wrapper_info = (struct wrapper_struct*)(info_);

     void *(*start_routine)(void*) = wrapper_info->start_routine;
     void* arg = wrapper_info->arg;
     int thread_id = wrapper_info->thread_id;

     tls_index = thread_id;
     dbgprintf("new thread tls_index: %d\n",tls_index);

     plocklib_release_simple_lock(&id_lock);
     plocklib_acquire_simple_lock(&threads[tls_index].threadlock);

     void* to_return = start_routine(arg);

     //Something is rotten in Denmark.
     //This shouldn't be necessary, and I'm not sure if it actually does anything,
     //but all multithreaded programs seem to leak memory on Linux, and I noticed
     //that pthread_exit wasn't being called automatically like it's supposed to be,
     //so I'm calling it here.
     //Further investigation is needed, though.
     pthread_exit(to_return);
}

#ifndef SPECIALSNOWFLAKE
static int (*real_pthread_create)(pthread_t * thread,
              const pthread_attr_t * attr,
              void *(*start_routine)(void*), void * arg) = NULL;
#else
static int (*real_pthread_create)(pthread_t * thread,
              const pthread_attr_t * attr,
              void *(*start_routine)(void*), void * arg) = pthread_create;
#endif

#ifndef SPECIALSNOWFLAKE
static void (*real_pthread_exit)(void* retval) = NULL;
#else
static void (*real_pthread_exit)(void* retval) = pthread_exit;
#endif

static int client_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg)
{
     int this_thread_id;

     plocklib_acquire_simple_lock(&id_lock);
     while(threads[next_thread_id].threadlock)
     {
    	 next_thread_id++;
    	 next_thread_id%=PALLOC_MAX_THREADS;
     }
     this_thread_id = next_thread_id;
     next_thread_id++;
     next_thread_id%=PALLOC_MAX_THREADS;

     static struct wrapper_struct thread_info;
     thread_info.start_routine = start_routine;
     thread_info.arg = arg;
     thread_info.thread_id = this_thread_id;

     return real_pthread_create(thread,attr,wrapped_startthread,(void*)(&thread_info));
}

static void client_pthread_exit(void *retval)
{
     dbgprintf("begin client_pthread_exit\n");
	/*Clean up*/
	int i;
	for(i=0; i<NUM_PALLOC_BUCKETS; i++)
		if(threads[tls_index].buckets[i])
			/*If a bucket is totally free, munmap it.*/
			if(threads[tls_index].buckets[i]->free_entries + threads[tls_index].buckets[i]->prefilled_entries == PALLOC_PAGE_ENTRIES)
			{
				struct page_record* to_delete = threads[tls_index].buckets[i];
				threads[tls_index].buckets[i] = threads[tls_index].buckets[i]->chain_forward_ptr;
				munmap(to_delete,MIN_SUPERPAGE_SIZE << min(i,PALLOC_HACK_MAX_SIZE_CLASS));
			}

	/*Make ourselves a free thread record*/
	plocklib_release_simple_lock(&threads[tls_index].threadlock);

    dbgprintf("end client_pthread_exit\n");
	real_pthread_exit(retval);
}

#ifndef SPECIALSNOWFLAKE
int pthread_create(pthread_t * thread,
		   const pthread_attr_t * attr,
		   void *(*start_routine)(void*), void * arg)
{
     if(!real_pthread_create)
     {
	  real_pthread_create = dlsym(((void *) -1l),"pthread_create");
     }

     return client_pthread_create(thread,attr,start_routine,arg);
}
#else
#define pthread_create client_pthread_create
#endif

#ifndef SPECIALSNOWFLAKE
void pthread_exit(void* retval)
{
     if(!real_pthread_exit)
     {
	  real_pthread_exit = dlsym(((void *) -1l),"pthread_exit");
     }

     client_pthread_exit(retval);
}
#else
#define pthread_exit client_pthread_exit
#endif

#endif
