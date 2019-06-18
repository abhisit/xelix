/* pipe.c: Inter-process pipes
 * Copyright © 2019 Lukas Martini
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

#include "pipe.h"
#include <fs/vfs.h>
#include <errno.h>
#include <tasks/task.h>
#include <mem/kmalloc.h>

// FIXME Should dynamically grow
#define PIPE_BUFFER_SIZE 0x5000

struct pipe {
	void* buffer;
	uint32_t data_size;
	int fd[2];
};

size_t pipe_read(struct vfs_callback_ctx* ctx, void* dest, size_t size) {
	struct pipe* pipe = (struct pipe*)ctx->fp->mount_instance;

	if(!pipe->data_size && ctx->fp->flags & O_NONBLOCK) {
		sc_errno = EAGAIN;
		return -1;
	}

	vfs_file_t* write_fp = vfs_get_from_id(pipe->fd[1], ctx->task);
	if(!pipe->data_size && !write_fp) {
		sc_errno = EBADF;
		return -1;
	}

	while(!pipe->data_size) {
		halt();
	}

	if(size > pipe->data_size) {
		size = pipe->data_size;
	}

	memcpy(dest, pipe->buffer, size);
	pipe->data_size -= MIN(pipe->data_size, size);
	if(pipe->data_size) {
		memmove(pipe->buffer, pipe->buffer + size, pipe->data_size);
	}

	return size;
}

size_t pipe_write(struct vfs_callback_ctx* ctx, void* source, size_t size) {
	struct pipe* pipe = (struct pipe*)ctx->fp->mount_instance;
	if(pipe->data_size + size > PIPE_BUFFER_SIZE) {
		sc_errno = EFBIG;
		return -1;
	}

	memcpy(pipe->buffer + pipe->data_size, source, size);
	pipe->data_size += size;
	return size;
}

int vfs_pipe(int fildes[2], task_t* task) {
	vfs_file_t* fd1 = vfs_alloc_fileno(task, 3);
	if(!fd1) {
		sc_errno = EMFILE;
		return -1;
	}

	vfs_file_t* fd2 = vfs_alloc_fileno(task, fd1->num);
	if(!fd2) {
		vfs_close(fd1->num, task);
		sc_errno = EMFILE;
		return -1;
	}

	struct pipe* pipe = zmalloc(sizeof(struct pipe));
	pipe->buffer = zmalloc(PIPE_BUFFER_SIZE);
	pipe->fd[0] = fd1->num;
	pipe->fd[1] = fd2->num;

	fd1->callbacks.read = pipe_read;
	fd2->callbacks.write = pipe_write;
	fd1->flags = O_RDONLY;
	fd2->flags = O_WRONLY;
	fd1->mount_instance = (void*)pipe;
	fd2->mount_instance = (void*)pipe;
	fd1->type = FT_IFPIPE;
	fd2->type = FT_IFPIPE;

	fildes[0] = fd1->num;
	fildes[1] = fd2->num;
	return 0;
}
