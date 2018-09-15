/* fork.c: Fork Syscall
 * Copyright © 2014-2015 Lukas Martini
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

#include <log.h>
#include <tasks/syscall.h>
#include <tasks/task.h>

SYSCALL_HANDLER(fork)
{
	task_t* fork_task = task_fork(syscall.task, syscall.state);

	if(fork_task == NULL) {
		return -1;
	}

	scheduler_add(fork_task);
	return fork_task->pid;
}

