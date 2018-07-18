#pragma once

/* Copyright © 2011-2018 Lukas Martini
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

#include <lib/generic.h>

void* __attribute__((alloc_size(1))) _kmalloc(size_t sz, bool align, const char* _debug_file, uint32_t _debug_line, const char* _debug_func);
void _kfree(void *ptr, const char* _debug_file, uint32_t _debug_line, const char* _debug_func);

#define kmalloc(sz) _kmalloc(sz, false, __FILE__, __LINE__, __FUNCTION__)
#define kmalloc_a(sz) _kmalloc(sz, true, __FILE__, __LINE__, __FUNCTION__)
#define kfree(ptr) _kfree(ptr, __FILE__, __LINE__, __FUNCTION__)

void kmalloc_init();
