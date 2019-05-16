#pragma once

/* Copyright © 2011 Fritz Grimpen
 * Copyright © 2013-2019 Lukas Martini
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

#include <int/int.h>
#include <stdbool.h>

#define PAGE_SIZE 4096

/* Internal representation of a page allocation. This will get mapped to the
 * hardware form by <arch>-paging.c.
 */
struct vmem_range {
	struct vmem_range* next;
	bool readonly:1;
	bool cow:1;
	bool allocated:1;
	bool user:1;

	void* cow_src;
	uintptr_t virt_start;
	uintptr_t phys_start;
	uintptr_t length;
};

struct vmem_context {
	struct vmem_range* first_range;
	struct vmem_range* last_range;
	uint32_t num_ranges;

	// Address of the actual page tables that will be read by the hardware
	void* tables;
};

struct vmem_context* vmem_kernelContext;

void vmem_map(struct vmem_context* ctx, void* virt_start, void* phys_start, uintptr_t size, bool user, bool ro);
#define vmem_map_flat(ctx, start, size, user, ro) vmem_map(ctx, start, start, size, user, ro)
uintptr_t vmem_translate(struct vmem_context* ctx, uintptr_t raddress, bool reverse);
void vmem_rm_context(struct vmem_context* ctx);
void vmem_init();
