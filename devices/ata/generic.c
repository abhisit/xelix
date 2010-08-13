/*  devices/ata/generic.c: Generic driver for ATA harddisks
 *  This file is part of Xelix. The license in COPYING applies to this file. If you did not receive such a file along with this code, you can get it from http://xelix.org.
 *  Written by:
 *  - Lukas Martini
 *  Todo:
 *  - Keep track of bad sectors
 *  Notes:
 *  - Always read the status of a drive first before sending any data. Sending something may also modify the status, resulting in loosing the ability to check if there is any drive.
 *  - All the preprocessor variables are defined in generic.h
 */

#include <devices/ata/interface.h>
#include <devices/ata/generic.h>
#include <memory/kmalloc.h>

int selectedDrive = -1;
ataDrive_t* drive0;
ataDrive_t* drive1;

void delay();
void setActiveDrive();
uint8* getDriveStatus();
void flushCache();

// Read the Status 4 times, resulting in a 400 nanoseconds delay [one cpu io port reading takes something about 100ns]. As supposed in the ATA specifications.
void delay()
{
	int i;
	for(i = 0; i < 4; i++)
		getDriveStatus();
}
// Get the drive status. Should be self-explaining.
uint8* getDriveStatus()
{
	uint8* status = kmalloc(sizeof(uint8));
	*status = inb(STATUS_PORT);
	return status;
}

// Select the active drive we want to use on one controller [0/1].
void setActiveDrive(int drive)
{
	int value;
	ASSERT(drive > -1 && drive < 2); // Only 0 and 1 are possible
	if(selectedDrive == drive) return;
	if(!drive) value = 0xA0; // Master
	else value = 0xB0; // Slave
	outb(SELECT_PORT, value);
	delay(); // Now give the controller a bit time for setting the selected drive.
	selectedDrive = drive;
}

// Flush the write cache. Normally, the drive should do this automatically, but for support of old ones, we also do it manually.
void flushCache()
{

}

// Find out if there are actually any ATA drives.
void ata_detectDrives()
{
	int i;
	for(i=0; i < 2; i++)
	{
		setActiveDrive(i);
		outb(COMMAND_PORT, 0xEC); // IDENTIFY
		uint8* driveStatus = getDriveStatus(); // Now read the return value
		if(!*driveStatus)
		{
			if(!i)
			{
				log("Didn't find a master drive\n");
				drive0 = NULL;
			} else {
				log("Didn't find a slave drive\n");
				drive1 = NULL;
			}
		} else {
			if(!i)
			{
				log("Found a master drive\n");
				drive0 = kmalloc(sizeof(ataDrive_t));
				drive0->num = 0;
				drive0->blocked = 1;
			} else {
				log("Found a slave drive\n");
				drive1 = kmalloc(sizeof(ataDrive_t));
				drive1->num = 1;
				drive1->blocked = 1;
			}
			print("    Status: ");
			printDec(*driveStatus);
			print("\n");
		}	
	}
}

// Now init all this stuff. Called by init/main.c
void ata_init()
{
	log("Detecting ATA drives...\n");
	ata_detectDrives();
	if(drive0 == NULL && drive1 == NULL) return; // Nothing to do

	int i;
	for(i=0; i < 2; i++)
	{
		if(!i && drive0 == NULL) continue;
		if(i && drive1 == NULL) continue;
		
		log("Initializing drive #");
		logDec(i);
		log("\n");
		
		setActiveDrive(i);

		// Wait until device(s) get(s) ready
		uint8* status;
		while((status = getDriveStatus())){
			if(*status) PANIC("ATA device error"); // 1 = Error
			if(*status == 8) break; // Ok, device is ready
		}
		
	}
}
