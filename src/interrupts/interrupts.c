/* interrupts.c: Initialization of and interface to interrupts.
 * Copyright © 2010 Christoph Sünderhauf
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

#include "interface.h"
#include <common/log.h>
#include <common/generic.h>
#include <interrupts/idt.h>

interruptHandler_t interruptHandlers[256];

void interrupt_callback(registers_t regs)
{
	// That might look useless, but trust me, it isn't.
	static bool inInterrupt = false;
	
	if(inInterrupt)
		return; // Drop interrupt
	inInterrupt = true;
	
	if (interruptHandlers[regs.int_no] != 0)
	{
		interruptHandler_t handler = interruptHandlers[regs.int_no];
		handler(regs);
	}
	
	inInterrupt = false;
}

// Send EOI (end of interrupt) signals to the PICs.
static void sendEOI(bool slave)
{
	if (slave)
		outb(0xA0, 0x20);
	else
		outb(0x20, 0x20);
}

// This gets called from our ASM irq handler stub.
void irq_handler(registers_t regs)
{
	// If this interrupt involved the slave, send a EOI to the slave.
	if (regs.int_no >= 40)
		sendEOI(true);

	sendEOI(false); // Master
	interrupt_callback(regs);
}

void interrupt_registerHandler(uint8 n, interruptHandler_t handler)
{
	interruptHandlers[n] = handler;
	log("interrupts: Registered IRQ handler for %d.\n", n);
}

void interrupts_init()
{
	idt_init();

	// set all interruptHandlers to zero
	memset(interruptHandlers, 0, 256*sizeof(interruptHandler_t));
	log("interrupts: Initialized\n");
}
