#pragma once

/* Copyright © 2010 Christoph Sünderhauf
 * Copyright © 2011 Lukas Martini
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
#include <hw/cpu.h>

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

#define interrupts_disable() asm volatile("cli")
#define interrupts_enable() asm volatile("sti")

typedef void (*interrupt_handler_t)(cpu_state_t*);

cpu_state_t* interrupts_callback(cpu_state_t* regs);
struct vmem_context* interrupts_get_previous_context();
void interrupts_registerHandler(uint8_t n, interrupt_handler_t handler);
void interrupts_bulkRegisterHandler(uint8_t start, uint8_t end, interrupt_handler_t handler);
void interrupts_init();
