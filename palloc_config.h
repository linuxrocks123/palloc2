#ifndef PALLOC_CONFIG_H
#define PALLOC_CONFIG_H

/*Don't let the "config" name confuse you.
  Mortals should not mess with this.
  Or Texas.*/

/*Generally useful stuff.*/
#define min(x1,x2) ((x1) > (x2) ? (x2):(x1))
#define max(x1,x2) ((x1) < (x2) ? (x2):(x1))
#define bits_in(t) (sizeof(t)*8)
#define ALIGN_SIZE(size,grain) (((grain)+(size)-1) & ~((grain)-1))

/*This was copied from the Linux kernel*/
#ifndef __SUNPRO_C
static inline uint64_t fls64(uint64_t word)
{
	asm("bsr %1,%0"
	    : "=r" (word)
	    : "rm" (word));
	return word;
}
#else
static inline int fls(int x)
{
        int r = 32;

        if (!x)
                return 0;
        if (!(x & 0xffff0000u)) {
                x <<= 16;
                r -= 16;
        }
        if (!(x & 0xff000000u)) {
                x <<= 8;
                r -= 8;
        }
        if (!(x & 0xf0000000u)) {
                x <<= 4;
                r -= 4;
        }
        if (!(x & 0xc0000000u)) {
                x <<= 2;
                r -= 2;
        }
        if (!(x & 0x80000000u)) {
                x <<= 1;
                r -= 1;
        }
        return r;
}

static inline int fls64(uint64_t x)
{
        uint32_t h = x >> 32;
        if (h)
                return fls(h) + 32;
        return fls(x);
}
#endif

static inline uint64_t flz64(uint64_t word)
{
	return fls64(~word);
}

static inline int popcount(uint64_t word)
{
	return __builtin_popcountl(word);
}

/*This is enough to support up to 16GB allocations.
  If you do need to increase this, you may need to
  take more bits out of the address space as well.
  It actually probably just won't work.
*/
#define NUM_PALLOC_BUCKETS 31

/*This must be a multiple of 64 or I will throw a sheep at you.*/
#define PALLOC_PAGE_ENTRIES 512
#define PALLOC_BITVEC_ENTRIES ( PALLOC_PAGE_ENTRIES / bits_in(uint64_t) )

#define MIN_SIZE_CLASS 8
const static int MIN_SET_BIT = /*fls64(MIN_SIZE_CLASS) = */ 3;
#define MIN_SUPERPAGE_SIZE ( (int64_t) (MIN_SIZE_CLASS * PALLOC_PAGE_ENTRIES) )

#define PALLOC_MAX_THREADS 128

/*This is how much memory to mmap per page_record structure.*/
#define PALLOC_RECORD_FREELIST_CHUNK_SIZE 65536

/*Hack to support malloc of very large allocations*/
#define PALLOC_HACK_MAX_SIZE_CLASS 18

/*Hack to support malloc of very, very large allocations*/
#define PALLOC_HACK_ABSURDLY_HUGE_SIZE_CLASS 21
#define PALLOC_HACK_SINGLETON_MMAP_OFFSET 9 /*log_2(MIN_SUPERPAGE_SIZE)-3*/

#endif
