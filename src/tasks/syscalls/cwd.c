/* cwd.c: Get/set current working directory
 * Copyright © 2013-2015 Lukas Martini
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

#include <tasks/syscall.h>
#include <tasks/task.h>
#include <fs/vfs.h>
#include <string.h>

SYSCALL_HANDLER(chdir)
{
	SYSCALL_SAFE_RESOLVE_PARAM(0);

	vfs_file_t* fp = vfs_open((char*)syscall.params[0], syscall.task);
	if(!fp) {
		return -1;
	}

	strncpy(syscall.task->cwd, fp->path, TASK_PATH_MAX);
	return 0;
}

SYSCALL_HANDLER(getcwd)
{
	SYSCALL_SAFE_RESOLVE_PARAM(0);

	// Maximum return string size
	if(syscall.params[1] > TASK_PATH_MAX)
		syscall.params[1] = TASK_PATH_MAX;

	memcpy((char*)syscall.params[0], syscall.task->cwd, syscall.params[1]);
	return syscall.params[0];
}
