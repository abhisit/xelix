#pragma once

/* Copyright © 2018-2019 Lukas Martini
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

#include <printf.h>
#include <fs/vfs.h>

#define sysfs_printf(args...) rsize += snprintf(dest + rsize, size - rsize, args);

struct sysfs_file {
	char name[40];
	struct vfs_callbacks cb;
	void* meta;
	uint16_t type;
	struct sysfs_file* next;
	struct sysfs_file* prev;
};

struct sysfs_file* sysfs_add_file(char* name, struct vfs_callbacks* cb);
struct sysfs_file* sysfs_add_dev(char* name, struct vfs_callbacks* cb);
void sysfs_rm_file(char* name);
void sysfs_rm_dev(char* name);
void sysfs_init();

vfs_file_t* sysfs_open(char* path, uint32_t flags, void* mount_instance, task_t* task);
int sysfs_stat(char* path, vfs_stat_t* dest, void* mount_instance, task_t* task);
int sysfs_access(char* path, uint32_t amode, void* mount_instance, struct task* task);
int sysfs_readlink(const char* path, char* buf, size_t size, void* mount_instance, struct task* task);
