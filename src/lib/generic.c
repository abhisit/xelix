/* generic.c: Common utilities often used.
 * Copyright © 2010 Lukas Martini, Christoph Sünderhauf
 * Copyright © 2011-2018 Lukas Martini
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

#include "generic.h"

#include "log.h"
#include "string.h"
#include "print.h"
#include <memory/kmalloc.h>
#include <hw/serial.h>
#include <hw/pit.h>
#include <hw/keyboard.h>
#include <tasks/scheduler.h>

char* itoa(int value, char* result, int base)
{
	if (base < 2 || base > 36) { *result = '\0'; return result; }

	char* ptr = result, *ptr1 = result, tmp_char;
	int tmp_value;

	do {
		tmp_value = value;
		value /= base;
		*ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + (tmp_value - value * base)];
	} while ( value );

	// Apply negative sign
	if(tmp_value < 0) *ptr++ = '-';
	*ptr-- = '\0';

	while(ptr1 < ptr) {
		tmp_char = *ptr;
		*ptr--= *ptr1;
		*ptr1++ = tmp_char;
	}

	return result;
}

char* utoa(unsigned int value, char* result, int base)
{
	if(base < 2 || base > 36) {
		*result = '\0';
		return result;
	}

	if(value == 0) {
		result[0] = '0';
		result[1] = '\0';
		return result;
	}

	char* ptr = result;

	for(; value; value /= base) {
		*ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[value % base];
	}

	return result;
}

uint64_t atoi(const char* s) {
	uint64_t n = 0;
	while (isCharDigit(*s)) n = 10 * n + *s++ - '0';
	return n;
}
