/* elf.c: Loader for ELF binaries
 * Copyright © 2011-2019 Lukas Martini
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

#include "elf.h"

#include <log.h>
#include <print.h>
#include <string.h>
#include <errno.h>
#include <tasks/task.h>
#include <fs/vfs.h>
#include <mem/kmalloc.h>
#include <mem/track.h>
#include <mem/vmem.h>

#ifdef ELF_DEBUG
 #define debug(args...) log(LOG_DEBUG, args);
#else
 #define debug(...)
#endif

#define MAXDEPS 50

static char elf_magic[16] = {0x7f, 'E', 'L', 'F', 01, 01, 01, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static inline void* bin_read(int fd, size_t offset, size_t size, void* inbuf) {
	void* buf = inbuf ? inbuf : kmalloc(size);
	vfs_seek(fd, offset, VFS_SEEK_SET, NULL);
	size_t read = vfs_read(fd, buf, size, NULL);
	if(likely(read == size)) {
		return buf;
	}

	debug("elf: bin_read: Read size %#x at offset %#x fd %d smaller than expected %#x\n", read, offset, fd, size);
	if(!inbuf) {
		kfree(buf);
	}
	return NULL;
}

static int load_phead(task_t* task, int fd, elf_program_header_t* phead, bool is_main) {
	int section = TMEM_SECTION_DATA;
	if(phead->flags & PF_X) {
		// Can't be both executable and writable w/ current vmem setup
		if(phead->flags & PF_W) {
			return -1;
		}
		section = TMEM_SECTION_CODE;
	}

	size_t size = VMEM_ALIGN(phead->memsz);
	void* phys = zmalloc_a(size);
	if(unlikely(!phys)) {
		return -1;
	}

	void* virt = phys;
	size_t phys_offset = 0;

	if(is_main) {
		virt = VMEM_ALIGN_DOWN(phead->vaddr);
		phys_offset = phead->vaddr - virt;

		if(virt + size > task->sbrk) {
			task->sbrk = virt + size;
		}
	}

	if(unlikely(!bin_read(fd, phead->offset, phead->filesz, phys + phys_offset))) {
		kfree(phys);
		return -1;
	}

	task_add_mem(task, virt, phys, size, section, TASK_MEM_FORK | TASK_MEM_FREE);

	debug("  phys %#-8x-%#-8x virt %#-8x-%#-8x\n",
		phys, (intptr_t)phys + size, virt, (intptr_t)virt + size);
	return 0;
}

static int read_dyn_table(struct elf_load_ctx* ctx, int fd, elf_program_header_t* phead) {
	void* dyn = bin_read(fd, phead->offset, phead->filesz, NULL);
	if(!dyn) {
		return -1;
	}

	elf_dyn_tag_t* tag = (elf_dyn_tag_t*)dyn;
	while((void*)(tag + 1) < dyn + phead->filesz && tag->tag) {
		switch(tag->tag) {
			#if 0
			case DT_NEEDED:
				if(ctx->ndyndeps >= MAXDEPS) {
					return -1;
				}

				ctx->dyndeps[ctx->ndyndeps++] = tag->val;
				break;
			#endif
			case DT_STRTAB:
				ctx->dynstrtab = (void*)tag->val;
				break;
		}
		tag++;
	}

	kfree(dyn);
	return 0;
}

static uint32_t read_pheads(task_t* task, int fd, elf_t* header, bool is_main) {
	elf_program_header_t* phead_start = bin_read(fd, header->phoff,
		header->phnum * header->phentsize, NULL);
	if(unlikely(!phead_start)) {
		return -1;
	}

	elf_program_header_t* phead = phead_start;

	debug("elf: Program headers:\n");
	for(int i = 0; i < header->phnum; i++) {
		debug("  %-2d type %-2d offset %#-6x vaddr %#-8x memsz %#-8x filesz %#-8x\n",
			i, phead->type, phead->offset, phead->vaddr, phead->memsz, phead->filesz);

		switch(phead->type) {
			case PT_LOAD:
				if(load_phead(task, fd, phead, is_main) < 0) {
					kfree(phead_start);
					return -1;
				}
				break;
			case PT_INTERP:
				if(is_main) {
					task->elf_ctx.interp = bin_read(fd, phead->offset, phead->filesz, NULL);
					kfree(phead_start);
					return task->elf_ctx.interp ? 0 : -1;
				}
				break;
			case PT_DYNAMIC:
				if(is_main) {
					if(read_dyn_table(task, fd, phead) < 0) {
						kfree(phead_start);
						return -1;
					}
				}
				break;
		}

		phead = (elf_program_header_t*)((intptr_t)phead + header->phentsize);
	}
	kfree(phead_start);
	return 0;
}

#define LF_ASSERT(cmp, msg)                    \
if(unlikely(!(cmp))) {                         \
	log(LOG_INFO, "elf: elf_load: " msg "\n"); \
	vfs_close(fd, NULL);                       \
	kfree(header);                             \
	return -1;                                 \
}

static int load_file(task_t* task, char* path, bool is_main) {
	debug("elf: Loading %s\n", path);
	int fd = vfs_open(path, O_RDONLY, NULL);
	if(unlikely(fd < 0)) {
		return -1;
	}

	elf_t* header = bin_read(fd, 0, sizeof(elf_t), NULL);
	LF_ASSERT(header, "Could not read ELF header");
	LF_ASSERT(!memcmp(header->ident, elf_magic, sizeof(elf_magic)),
		"Invalid magic");

	if(is_main) {
		LF_ASSERT(header->type == ELF_TYPE_EXEC, "Binary is inexecutable");
	}

	LF_ASSERT(header->machine == ELF_ARCH_386, "Invalid architecture");
	LF_ASSERT(header->version == ELF_VERSION_CURRENT, "Unsupported ELF version");
	LF_ASSERT(header->entry, "Binary has no entry point");
	LF_ASSERT(header->phnum, "No program headers");
	LF_ASSERT(header->shnum, "No section headers");

	LF_ASSERT(read_pheads(task, fd, header, is_main) != -1, "Loading program headers failed");

	if(is_main) {
		task->entry = header->entry;
	}

	kfree(header);
	vfs_close(fd, NULL);
	return 0;
}

int elf_load_file(task_t* task, char* path) {
	char* abs_path = vfs_normalize_path(path, task->cwd);
	strncpy(task->binary_path, abs_path, TASK_PATH_MAX);

	if(load_file(task, abs_path, true) < 0) {
		kfree(abs_path);
		sc_errno = ENOEXEC;
		return -1;
	}
	kfree(abs_path);

	if(task->elf_ctx.interp) {
		if(load_file(task, task->elf_ctx.interp, true) < 0) {
			sc_errno = ENOEXEC;
			return -1;
		}
	}

/*
	if(ctx->ndyndeps) {
		char* dynstr = (void*)vmem_translate(task->memory_context, (intptr_t)ctx->dynstrtab, false);
		for(int i = 0; i < ctx->ndyndeps; i++) {
			char* lib_path = kmalloc(strlen(dynstr + ctx->dyndeps[i]) + 10);
			sprintf(lib_path, "/usr/lib/%s", dynstr + ctx->dyndeps[i]);

			if(load_file(task, lib_path, false) < 0) {
				kfree(lib_path);
				sc_errno = ENOEXEC;
				return -1;
			}
			kfree(lib_path);
		}
	}
*/
	task_set_initial_state(task);

	debug("elf: Entry point 0x%x, sbrk 0x%x\n", task->entry, task->sbrk);
	return 0;
}
