#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <asm/page.h>

struct page_counter {
	atomic_long_t count;
	unsigned long limit;
	struct page_counter *parent;

	/* legacy */
	unsigned long watermark;
	unsigned long failcnt;

	/*the limit that triggers per memcg reclaim.*/
	unsigned long long low_wmark_limit;
	/*the limit that stops per memcg reclaim.*/
	unsigned long long high_wmark_limit;
};

#if BITS_PER_LONG == 32
#define PAGE_COUNTER_MAX LONG_MAX
#else
#define PAGE_COUNTER_MAX (LONG_MAX / PAGE_SIZE)
#endif

#define CHARGE_WMARK_LOW       0x01
#define CHARGE_WMARK_HIGH      0x02

static inline void page_counter_init(struct page_counter *counter,
				     struct page_counter *parent)
{
	atomic_long_set(&counter->count, 0);
	counter->limit = PAGE_COUNTER_MAX;
	counter->low_wmark_limit = PAGE_COUNTER_MAX;
	counter->high_wmark_limit = PAGE_COUNTER_MAX;
	counter->parent = parent;
}

static inline unsigned long page_counter_read(struct page_counter *counter)
{
	return atomic_long_read(&counter->count);
}

void page_counter_cancel(struct page_counter *counter, unsigned long nr_pages);
void page_counter_charge(struct page_counter *counter, unsigned long nr_pages);
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long nr_pages,
			     struct page_counter **fail);
void page_counter_uncharge(struct page_counter *counter, unsigned long nr_pages);
int page_counter_limit(struct page_counter *counter, unsigned long limit);
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages);


static inline int
page_counter_set_high_wmark_limit(struct page_counter *counter,
		unsigned long long wmark_limit)
{
	xchg(&counter->high_wmark_limit, wmark_limit);
	return 0;
}

static inline int
page_counter_set_low_wmark_limit(struct page_counter *counter,
		unsigned long long wmark_limit)
{
	xchg(&counter->low_wmark_limit, wmark_limit);
	return 0;
}


static inline void page_counter_reset_watermark(struct page_counter *counter)
{
	counter->watermark = page_counter_read(counter);
}

static inline bool
page_counter_check_under_low_wmark_limit(struct page_counter *counter)
{
	return (atomic_long_read(&counter->count) * PAGE_SIZE
						 < counter->low_wmark_limit);
}

static inline bool
page_counter_check_under_high_wmark_limit(struct page_counter *counter)
{
	return (atomic_long_read(&counter->count) * PAGE_SIZE
						< counter->high_wmark_limit);
}


#endif /* _LINUX_PAGE_COUNTER_H */
