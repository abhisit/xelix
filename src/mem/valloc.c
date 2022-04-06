/* valloc.c: Virtual memory allocator
 * Copyright © 2020-2022 Lukas Martini
 *
 * This file is part of Xelix.
 *
 * Xelix is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Xelix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xelix. If not, see <http://www.gnu.org/licenses/>.
 */

#include "valloc.h"
#include <mem/paging.h>
#include <mem/kmalloc.h>
#include <mem/mem.h>
#include <boot/multiboot.h>
#include <string.h>
#include <bitmap.h>
#include <panic.h>
#include <spinlock.h>

static vmem_t malloc_ranges[50];
static int have_malloc_ranges = 50;

#define VALLOC_DEBUG 1

#ifdef VALLOC_DEBUG
	#define debug(args...) { if(flags & VM_DEBUG) { log(LOG_DEBUG, args); } }
#else
	#define debug(...)
#endif

static inline void* alloc_virt(struct valloc_ctx* ctx, size_t size, void* request) {
	uint32_t page_num;
	void* virt;
	if(request) {
		// FIXME Do duplicate checks
		virt = ALIGN_DOWN(request, PAGE_SIZE);
		page_num = (uintptr_t)virt / PAGE_SIZE;
	} else {
		page_num = bitmap_find(&ctx->bitmap, size);

		if(page_num == -1) {
			return NULL;
		}

		virt = (void*)(page_num * PAGE_SIZE);
	}

	bitmap_set(&ctx->bitmap, page_num, size);
	return virt;
}

static inline vmem_t* get_range(struct valloc_ctx* ctx, void* addr, bool phys) {
	if(!phys && !bitmap_get(&ctx->bitmap, (uintptr_t)addr / PAGE_SIZE)) {
		return NULL;
	}

	vmem_t* range = ctx->ranges;
	for(; range; range = range->next) {
		void* start = (phys ? range->phys : range->addr);
		if(addr >= start && addr < (start + range->size)) {
			return range;
		}
	}

	return NULL;
}

vmem_t* valloc_get_range(struct valloc_ctx* ctx, void* addr, bool phys) {
	if(!spinlock_get(&ctx->lock, -1)) {
		return NULL;
	}

	vmem_t* range = get_range(ctx, addr, phys);
	spinlock_release(&ctx->lock);
	return range;
}

static inline vmem_t* new_range() {
	/* During initialization, kmalloc_init calls valloc once to get its memory
	 * space to allocate from. The zmalloc call below would fail since kmalloc
	 * is not ready yet. Another call to valloc can then happen in
	 * paging_set_range when a new page table is allocated.
	 * Add a dirty hack for that one-time special case.
	 */

	// FIXME combine into simple early_alloc with the initial page dir allocation
	if(unlikely(!kmalloc_ready)) {
		if(likely(have_malloc_ranges)) {
			return &malloc_ranges[50 - have_malloc_ranges--];
		} else {
			panic("valloc: preallocated ranges exhausted before kmalloc is ready\n");
		}
	} else {
		return kmalloc(sizeof(vmem_t));
	}
}

int valloc_at(struct valloc_ctx* ctx, vmem_t* vmem, size_t size, void* virt_request, void* phys, int flags) {
	if(!spinlock_get(&ctx->lock, -1)) {
		return -1;
	}

	// FIXME Fail if size, virt_request or phys are not page aligned

	// Allocate virtual address
	void* virt = alloc_virt(ctx, size, virt_request);
	if(!virt) {
		spinlock_release(&ctx->lock);
		return -1;
	}

	//debug("ctx %p page_num %5d valloc %p size %#x\n", ctx, page_num, page_num * PAGE_SIZE, size * PAGE_SIZE);

	// Allocate physical address if necessary
	if(!phys) {
		phys = palloc(size);
		if(!phys) {
			spinlock_release(&ctx->lock);
			return -1;
		}
	}

	if(!(flags & VM_NO_MAP)) {
		if(ctx->page_dir) {
			paging_set_range(ctx->page_dir, virt, phys, size * PAGE_SIZE, flags);
		}
	}

	if(flags & VM_ZERO) {
		if(ctx == VA_KERNEL && !(flags & VM_NO_MAP)) {
			bzero(virt, size * PAGE_SIZE);
		} else {
			/* If the allocation is not in the kernel context or is set as NO_MAP,
			 * temporarily map it into the kernel virtual address space to zero it.
			 */
			if(ctx != VA_KERNEL) {
				if(!spinlock_get(&VA_KERNEL->lock, -1)) {
					return -1;
				}
			}

			int zero_page = bitmap_find(&VA_KERNEL->bitmap, size);
			if(zero_page == -1) {
				spinlock_release(&ctx->lock);
				if(ctx != VA_KERNEL) {
					spinlock_release(&VA_KERNEL->lock);
				}

				return -1;
			}

			void* zero_addr = (void*)(zero_page * PAGE_SIZE);
			paging_set_range(VA_KERNEL->page_dir, zero_addr, phys, size * PAGE_SIZE, VM_RW);
			bzero(zero_addr, size * PAGE_SIZE);
			paging_clear_range(VA_KERNEL->page_dir, zero_addr, size * PAGE_SIZE);

			if(ctx != VA_KERNEL) {
				spinlock_release(&VA_KERNEL->lock);
			}
		}
	}

	vmem_t* range = new_range();
	range->ctx = ctx;
	range->addr = virt;
	range->phys = phys;
	range->shards = NULL;
	range->size = size * PAGE_SIZE;
	range->flags = flags;
	range->self = range;

	range->previous = NULL;
	range->next = ctx->ranges;
	if(ctx->ranges) {
		ctx->ranges->previous = range;
	}
	ctx->ranges = range;

	if(vmem) {
		memcpy(vmem, range, sizeof(vmem_t));
	}

	debug("valloc %p -> %p size %#x\n", range->addr, range->phys, range->size);
	spinlock_release(&ctx->lock);
	return 0;
}

/* Transparently maps memory from one paging context into another.
 */
void* vmap(struct valloc_ctx* ctx, vmem_t* vmem, struct valloc_ctx* src_ctx,
	void* src_addr, size_t size, int flags) {

	if(!spinlock_get(&ctx->lock, -1) || !spinlock_get(&src_ctx->lock, -1)) {
		return NULL;
	}

	debug("vmap: %p size %#x\n", src_addr, size);
	void* src_aligned = ALIGN_DOWN(src_addr, PAGE_SIZE);
	size_t src_offset = (uintptr_t)src_addr % PAGE_SIZE;

	/* Get number of pages we have to allocate in the new virtual memory
	 * context. This can be larger than the requested size divided by the page
	 * size when src_addr is not page aligned - For example, when copying 0x100
	 * bytes from 0x1ff0, we need to allocate two pages to map both 0x1000 and
	 * 0x2000 for the full data, even though 0x100 < PAGE_SIZE.
	 */
	size_t size_pages = RDIV(size + src_offset, PAGE_SIZE);
	void* virt = alloc_virt(ctx, size_pages, NULL);
	if(!virt) {
		spinlock_release(&ctx->lock);
		spinlock_release(&src_ctx->lock);
		return NULL;
	}

	debug("  vmap: allocated %d pages at %p as target\n", size_pages, virt);

	vmem_t* range = new_range();
	range->ctx = ctx;
	range->addr = virt;
	range->phys = NULL;
	range->shards = NULL;
	range->size = size_pages * PAGE_SIZE;
	range->flags = flags;
	range->self = range;

	// Now go over the source ranges in passes and map as much as possible from each range
	// FIXME Currently only maps one page at a time
	//size_t range_offset = src_offset;
	size_t pages_offset = 0;
	int pages_mapped = 0;

	do {
		debug("  vmap: map pass %d for %p\n", pages_mapped, src_aligned + pages_offset);
		vmem_t* src_range = get_range(src_ctx, src_aligned + pages_offset, false);
		if(!src_range) {
			// FIXME Temp to map around broken execve
			if(flags & VM_MAP_UNDERALLOC_WORKAROUND) {
				pages_mapped++;
				break;
			}

			debug("No range!\n");
			spinlock_release(&ctx->lock);
			spinlock_release(&src_ctx->lock);
			return NULL;
		}


		if(!src_range->phys) {
			panic("valloc: Attempt to vmap sharded memory\n");
		}

		if(flags & VM_MAP_USER_ONLY && !(src_range->flags & VM_USER)) {
			return NULL;
		}

		// See how much we can map from this range
		//size_t range_offset = (uintptr_t)src_addr % PAGE_SIZE;

		//size_t to_copy = MIN(size - mapped, src_range->size - (range_offset % PAGE_SIZE));

		struct valloc_mem_shard* shard = kmalloc(sizeof(struct valloc_mem_shard));
		shard->addr = virt + pages_offset;
		shard->phys = src_range->phys + ALIGN_DOWN(src_addr - src_range->addr, PAGE_SIZE) + pages_offset;
		shard->next = range->shards;
		range->shards = shard;
		debug("vmapped %p -> %p\n", shard->addr, shard->phys);

		paging_set_range(ctx->page_dir, shard->addr, shard->phys, PAGE_SIZE, flags);

		pages_offset += PAGE_SIZE;
		pages_mapped++;
	} while(pages_mapped < size_pages);

	assert(pages_mapped == size_pages);

	if(vmem) {
		memcpy(vmem, range, sizeof(vmem_t));
	}

	// FIXME store range

	debug("\n");

	spinlock_release(&ctx->lock);
	spinlock_release(&src_ctx->lock);
	return virt + src_offset;
}

int vfree(vmem_t* range) {
	serial_printf("vfree!\n");
	struct valloc_ctx* ctx = range->ctx;
	spinlock_t* lock = &ctx->lock;
	if(!spinlock_get(lock, -1)) {
		return -1;
	}

	if(ctx->ranges == range->self) {
		ctx->ranges = range->next;
	}

	if(range->next) {
		range->next->previous = range->previous;
	}

	if(range->previous) {
		range->previous->next = range->next;
	}

	bitmap_clear(&ctx->bitmap, (uintptr_t)range->addr / PAGE_SIZE, RDIV(range->size, PAGE_SIZE));
	paging_clear_range(ctx->page_dir, range->addr, range->size);

	// FIXME VM_FREE should be the default
	if(range->phys && range->flags & VM_FREE) {
		serial_printf("vfree: freeing contig\n");
		pfree((uintptr_t)range->phys / PAGE_SIZE, RDIV(range->size, PAGE_SIZE));
	}

	struct valloc_mem_shard* shard = range->shards;
	while(shard) {
		serial_printf("vfree: freeing shard %p\n", shard);
		struct valloc_mem_shard* old = shard;
		if(range->flags & VM_FREE) {
			pfree((uintptr_t)shard->phys / PAGE_SIZE, RDIV(shard->size, PAGE_SIZE));
		}

		shard = old->next;
		serial_printf("vfree: next shard %p\n", shard);
		kfree(old);
	}

	kfree(range->self);
	spinlock_release(lock);
	return 0;
}

int valloc_new(struct valloc_ctx* ctx, struct paging_context* page_dir) {
	ctx->lock = 0;
	ctx->ranges = NULL;
	ctx->bitmap.data = ctx->bitmap_data;
	ctx->bitmap.size = PAGE_ALLOC_BITMAP_SIZE;
	bitmap_clear_all(&ctx->bitmap);

	// Don't allocate null pointer
	bitmap_set(&ctx->bitmap, 0, 1);

	if(page_dir) {
		ctx->page_dir = page_dir;
		ctx->page_dir_phys = page_dir;
	} else {
	/*	vmem_t vmem;
		valloc(VA_KERNEL, &vmem, 1, NULL, VM_RW | VM_ZERO);
		ctx->page_dir = vmem.addr;
		ctx->page_dir_phys = vmem.phys;
	*/
	}

	// Block NULL page
	bitmap_set(&ctx->bitmap, 0, 1);
	return 0;
}

void valloc_cleanup(struct valloc_ctx* ctx) {
	if(ctx->page_dir) {
		paging_rm_context(ctx->page_dir);
	}

	vmem_t* range = ctx->ranges;
	while(range) {
		if(range->flags & VM_FREE) {
			pfree((uintptr_t)range->phys / PAGE_SIZE, RDIV(range->size, PAGE_SIZE));
		}

		vmem_t* old_range = range;
		range = range->next;
		kfree(old_range);
	}
}

void* valloc_get_page_dir(struct valloc_ctx* ctx) {
	if(!ctx->page_dir) {
		vmem_t vmem;
		valloc(VA_KERNEL, &vmem, 1, NULL, VM_RW | VM_ZERO);
		ctx->page_dir = vmem.addr;
		ctx->page_dir_phys = vmem.phys;

		vmem_t* range = ctx->ranges;

		for(; range; range = range->next) {
			paging_set_range(ctx->page_dir, range->addr, range->phys, range->size, range->flags);
		}
	}
	return ctx->page_dir_phys;
}

int valloc_stats(struct valloc_ctx* ctx, uint32_t* total, uint32_t* used) {
	*total = ctx->bitmap.size * PAGE_SIZE;
	*used = bitmap_count(&ctx->bitmap) * PAGE_SIZE;
	return 0;
}
