/* kmalloc.c: Kernel memory allocator
 * Copyright © 2016-2019 Lukas Martini
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
 * along with Xelix.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kmalloc.h"
#include "track.h"
#include "vmem.h"
#include <log.h>
#include <string.h>
#include <multiboot.h>
#include <panic.h>
#include <spinlock.h>
#include <fs/sysfs.h>

#define GET_FOOTER(x) ((uint32_t*)((intptr_t)x + x->size + sizeof(struct mem_block)))
#define GET_CONTENT(x) ((void*)((intptr_t)x + sizeof(struct mem_block)))
#define GET_FB(x) ((struct free_block*)GET_CONTENT(x))
#define GET_HEADER_FROM_FB(x) ((struct mem_block*)((intptr_t)(x) - sizeof(struct mem_block)))
#define PREV_FOOTER(x) ((uint32_t*)((intptr_t)x - sizeof(uint32_t)))
#define PREV_BLOCK(x) ((struct mem_block*)((intptr_t)PREV_FOOTER(x) \
	- (*PREV_FOOTER(x)) - sizeof(struct mem_block)))
#define NEXT_BLOCK(x) ((struct mem_block*)((intptr_t)GET_FOOTER(x) + sizeof(uint32_t)))
#define FULL_SIZE(x) (x->size + sizeof(uint32_t) + sizeof(struct mem_block))

/* This is the block header struct. It is always located directly before the
 * start of the allocated area. Following the allocated area, there is a single
 * uint32_t containing the length of the block. As a result, any block header
 * except the first should always have the length of the previous block
 * directly before it. This makes it possible to use these blocks as a doubly
 * linked list.
 */
struct mem_block {
	#ifdef KMALLOC_CHECK
	uint16_t magic;
	#endif
	uint32_t size;

	enum {
		TYPE_USED,
		TYPE_FREE
	} type;
};

/* For free blocks, this struct gets stored inside the allocated area. As a
 * side effect of this, the minimum size for allocations is the size of this
 * struct.
 */
struct free_block {
	#ifdef KMALLOC_CHECK
	uint16_t magic;
	#endif
	struct free_block* prev;
	struct free_block* next;
};

/* If enabled, all block headers will begin with a magic which will be checked
 * during any modifications. Very useful to track down buffer overflows, but
 * comes with a performance penalty and wastes a bit of memory.
 */
#ifdef KMALLOC_CHECK
	#define KMALLOC_MAGIC 0xCAFE
	#define check_err(fmt) \
		log(LOG_ERR, "kmalloc: Metadata corruption at 0x%x: " fmt "\n", header);
	static void check_header(struct mem_block* header);
#endif

/* Enable debugging. This will send out cryptic debug codes to the serial line
 * during kmalloc()/free()'s. Also makes everything horribly slow. */
#ifdef KMALLOC_DEBUG
	char* _g_debug_file = "";
	#define debug(args...) { if(vmem_kernelContext \
		&& strcmp(_g_debug_file, "src/memory/vmem.c")) log(LOG_DEBUG, args); }
#else
	#define debug(args...)
#endif


bool kmalloc_ready = false;
static spinlock_t kmalloc_lock;
static struct free_block* last_free = (struct free_block*)NULL;
static intptr_t alloc_start;
static intptr_t alloc_end;
static intptr_t alloc_max;

static inline void unlink_free_block(struct free_block* fb) {
	if(fb->next) {
		fb->next->prev = fb->prev;
	}

	if(fb->prev) {
		fb->prev->next = fb->next;
	}

	if(fb == last_free) {
		last_free = fb->prev;
	}
}

static inline struct mem_block* set_block(size_t sz, struct mem_block* header) {
	header->size = sz;
	#ifdef KMALLOC_CHECK
	header->magic = KMALLOC_MAGIC;
	#endif

	// Add uint32_t footer with size so we can find the header
	*GET_FOOTER(header) = header->size;
	return header;
}

static struct mem_block* free_block(struct mem_block* header, bool check_next) {
	struct mem_block* prev = PREV_BLOCK(header);
	struct free_block* fb = GET_FB(header);

	/* If previous block is free, just increase the size of that block to also
	 * cover this area. Otherwise write free block metadata and add block.
	 */
	if((intptr_t)header > alloc_start && prev->type == TYPE_FREE) {
		#ifdef KMALLOC_CHECK
		header->magic = 0;
		#endif

		header = set_block(prev->size + FULL_SIZE(header), prev);
	} else {
		header->type = TYPE_FREE;

		fb->prev = last_free;
		fb->next = (struct free_block*)NULL;
		#ifdef KMALLOC_CHECK
		fb->magic = KMALLOC_MAGIC;
		#endif

		if(last_free) {
			last_free->next = fb;
		}

		last_free = fb;
	}

	// If next block is free, increase block size and unlink the next fb.
	struct mem_block* next = NEXT_BLOCK(header);
	if(check_next && alloc_end > (intptr_t)next && next->type == TYPE_FREE) {
		set_block(header->size + FULL_SIZE(next), header);
		unlink_free_block(GET_FB(next));
		#ifdef KMALLOC_CHECK
		next->magic = 0;
		#endif
	}

	return header;
}

static inline struct mem_block* split_block(struct mem_block* header, size_t sz) {
	// Make sure the block is big enough to get split first
	if(header->size < sz + sizeof(struct mem_block)
		+ sizeof(uint32_t) + sizeof(struct free_block)) {

		return NULL;
	}

	size_t orig_size = header->size;
	set_block(sz, header);
	size_t new_size = orig_size - sz - sizeof(struct mem_block) - sizeof(uint32_t);
	return set_block(new_size, NEXT_BLOCK(header));
}

static size_t get_alignment_offset(void* address) {
	size_t offset = 0;
	intptr_t content_addr = (intptr_t)GET_CONTENT(address);

	// Check if page is not already page aligned by circumstance
	if(content_addr & (PAGE_SIZE - 1)) {
		offset = VMEM_ALIGN(content_addr) - content_addr;

		/* We need at least x bytes to store the headers and footers of our
		 * block and of the new block we'll create in the offset
		 * FIXME Calc proper value for minimum size
	 	 */
		if(offset < 0x100) {
			offset += PAGE_SIZE;
		}
	}

	return offset;
}

static inline struct mem_block* get_free_block(size_t sz, bool align) {
	debug("FFB ");

	for(struct free_block* fb = last_free; fb; fb = fb->prev) {
		struct mem_block* fblock = GET_HEADER_FROM_FB(fb);

		#ifdef KMALLOC_CHECK
		check_header(fblock);
		#endif

		if(unlikely(fblock->type != TYPE_FREE)) {
			log(LOG_ERR, "kmalloc: Non-free block in free blocks linked list?\n");
			continue;
		}

		size_t sz_needed = sz;
		uint32_t alignment_offset = 0;

		/* For aligned blocks, special care needs to be taken as usually, the
		 * free block will have to be split up to an offset block and the
		 * actual allocation. This changes our space requirements – We now need
		 * a block with a content size big enough for the full size of the
		 * offset header (variable depending on address, but needs to be at
		 * least block header + footer size + minimum block size).
		 *
		 * Regardless of alignment, if our required size is smaller than the
		 * free block, we will split the free block into our allocation and a
		 * remainder. We also need to ensure the remainder is not smaller than
		 * the minimum block size.
		 */
		if(align) {
			alignment_offset = get_alignment_offset(fblock);
			sz_needed += alignment_offset + sizeof(struct mem_block) + sizeof(uint32_t);
		}

		if(fblock->size >= sz_needed) {
			debug("HIT 0x%x size 0x%x ", fblock, fblock->size);
			unlink_free_block(fb);

			// Carve a chunk of the required size out of the block
			struct mem_block* new = split_block(fblock, sz + alignment_offset);

			if(new) {
				// Already set this to prevent free_block from merging
				fblock->type = TYPE_USED;
				free_block(new, true);
			}

			return fblock;
		}
	}

	return NULL;
}

void* __attribute__((alloc_size(1))) _kmalloc(size_t sz, bool align, bool zero,
	char* _debug_file, uint32_t _debug_line, const char* _debug_func) {

	#ifdef KMALLOC_DEBUG
	_g_debug_file = _debug_file;
	#endif

	if(unlikely(!kmalloc_ready)) {
		panic("Attempt to kmalloc before allocator is kmalloc_ready.\n");
	}

	debug("kmalloc: %s:%d %s 0x%x ", _debug_file, _debug_line, _debug_func, sz);

	if(unlikely(sz < sizeof(struct free_block))) {
		sz = sizeof(struct free_block);
	}

	#ifdef KMALLOC_DEBUG
		if(sz >= (1024 * 1024)) {
			debug("(%d MB) ", sz / (1024 * 1024));
		} else if(sz >= 1024) {
			debug("(%d KB) ", sz / 1024);
		}
	#endif

	if(unlikely(!spinlock_get(&kmalloc_lock, 30))) {
		debug("Could not get spinlock\n");
		return NULL;
	}

	struct mem_block* header = get_free_block(sz, align);
	size_t sz_needed = sz;
	size_t alignment_offset = 0;

	if(align) {
		alignment_offset = get_alignment_offset(header ? header : (struct mem_block*)alloc_end);
	}

	if(!header) {
		debug("NEW ");

		if(alloc_end + sz_needed >= alloc_max) {
			panic("kmalloc: Out of memory");
		}

		if(align && alignment_offset) {
			sz_needed += get_alignment_offset((struct mem_block*)alloc_end);
		}

		header = set_block(sz_needed, (struct mem_block*)alloc_end);
		alloc_end = (uint32_t)GET_FOOTER(header) + sizeof(uint32_t);
	}

	if(align && alignment_offset) {
		debug("ALIGN off 0x%x ", alignment_offset);

		struct mem_block* new = split_block(header, alignment_offset
			- sizeof(struct mem_block) - sizeof(uint32_t));

		new->type = TYPE_USED;
		free_block(header, true);
		header = new;
	}

	header->type = TYPE_USED;
	spinlock_release(&kmalloc_lock);

	if(zero) {
		bzero((void*)GET_CONTENT(header), sz);
	}

	#ifdef KMALLOC_CHECK
	check_header(header);
	#endif

	#ifdef KMALLOC_CHECK
	check_header(header);
	#endif

	debug("RESULT 0x%x\n", (intptr_t)GET_CONTENT(header));
	return (void*)GET_CONTENT(header);
}

void _kfree(void *ptr, char* _debug_file, uint32_t _debug_line, const char* _debug_func) {
	if(!ptr) {
		return;
	}

	#ifdef KMALLOC_DEBUG
	_g_debug_file = _debug_file;
	#endif

	struct mem_block* header = (struct mem_block*)((intptr_t)ptr
		- sizeof(struct mem_block));

	debug("kfree: %s:%d %s 0x%x size 0x%x\n", _debug_file, _debug_line,
		_debug_func, ptr, header->size);
	if(unlikely((intptr_t)header < alloc_start ||
		(intptr_t)ptr >= alloc_end ||header->type == TYPE_FREE)) {

		log(LOG_ERR, "kmalloc: Attempt to free invalid block\n");
		return;
	}

	#ifdef KMALLOC_CHECK
	check_header(header);
	#endif

	if(unlikely(!spinlock_get(&kmalloc_lock, 30))) {
		debug("Could not get spinlock\n");
		return;
	}

	free_block(header, true);
	spinlock_release(&kmalloc_lock);
}

static size_t sfs_read(void* dest, size_t size, size_t offset, void* meta) {
	if(offset) {
		return 0;
	}

	size_t rsize = 0;
	uint32_t free = alloc_max - alloc_end;
	for(struct free_block* fb = last_free; fb; fb = fb->prev) {
		struct mem_block* fblock = GET_HEADER_FROM_FB(fb);
		free += fblock->size;
	}

	sysfs_printf("%d %d\n", alloc_max - alloc_start, free);
	return rsize;
}

void kmalloc_init() {
	memory_track_area_t* largest_area = NULL;
	for(int i = 0; i < memory_track_num_areas; i++) {
		memory_track_area_t* area = &memory_track_areas[i];

		if(area->type == MEMORY_TYPE_FREE &&
			(!largest_area || largest_area->size < area->size)) {

			largest_area = area;
		}
	}

	if(!largest_area) {
		panic("kmalloc: Could not find suitable memory area");
	}

	largest_area->type = MEMORY_TYPE_KMALLOC;
	alloc_start = alloc_end = (intptr_t)largest_area->addr;
	alloc_max = (intptr_t)largest_area->addr + largest_area->size;
	kmalloc_ready = true;
	sysfs_add_file("memfree", sfs_read, NULL);
}

#ifdef KMALLOC_CHECK
static void check_header(struct mem_block* header) {
	if(header->magic != KMALLOC_MAGIC) {
		check_err("Invalid magic");
	}

	if(header->size < sizeof(struct free_block)) {
		check_err("Block is smaller than minimum size");
	}

	if(*GET_FOOTER(header) != header->size) {
		check_err("Invalid footer");
	}

	if((intptr_t)header != alloc_start &&
		PREV_BLOCK(header)->magic != KMALLOC_MAGIC) {
		check_err("Previous block has invalid magic");
	}

	if(alloc_end > (intptr_t)header + FULL_SIZE(header) &&
		NEXT_BLOCK(header)->magic != KMALLOC_MAGIC) {
		check_err("Next block has invalid magic");
	}

	if(header->type == TYPE_FREE) {
		struct free_block* fb = GET_FB(header);
		if(unlikely(fb->magic != KMALLOC_MAGIC)) {
			check_err("Free block without free block metadata");
		}
	}
}
#endif

#ifdef KMALLOC_DEBUG
void kmalloc_stats() {
	struct mem_block* header = (struct mem_block*)alloc_start;
	log(LOG_DEBUG, "\nkmalloc_stats():\n");
	for(; (intptr_t)header < alloc_end; header = NEXT_BLOCK(header)) {

		#ifdef KMALLOC_CHECK
		check_header(header);
		if(header->magic != KMALLOC_MAGIC) {
			log(LOG_DEBUG, "0x%x\tcorrupted header\n", header);
			continue;
		}
		#endif

		log(LOG_DEBUG, "0x%x\tsize 0x%x\tres 0x%x\t", header, header->size,
				(intptr_t)header + sizeof(struct mem_block));
		log(LOG_DEBUG, "fsz 0x%x\tend 0x%x\t ", FULL_SIZE(header),
			(intptr_t)header + FULL_SIZE(header));

		if(header->type == TYPE_FREE) {
			struct free_block* fb = GET_FB(header);
			log(LOG_DEBUG, "free\tprev free: 0x%x next: 0x%x", fb->prev, fb->next);
		} else {
			log(LOG_DEBUG, "used");
		}

		log(LOG_DEBUG, "\n");
	}

	log(LOG_DEBUG, "\nalloc end:\t0x%x\n", alloc_end);
	log(LOG_DEBUG, "last free:\t0x%x\n\n", last_free);
}
#endif
