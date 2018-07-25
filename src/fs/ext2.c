/* ext2.c: Implementation of the extended file system, version 2
 * Copyright © 2013-2018 Lukas Martini
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

#ifdef ENABLE_EXT2

#include <lib/generic.h>
#include <lib/log.h>
#include <lib/string.h>
#include <lib/md5.h>
#include <memory/kmalloc.h>
#include <hw/ide.h>
#include <hw/serial.h>
#include <fs/vfs.h>
#include "ext2.h"

#ifdef EXT2_DEBUG
  #define debug(args...) serial_printf("ext2: " args)
#else
  #define debug(...)
#endif

typedef struct ext2_superblock {
	uint32_t inode_count;
	uint32_t block_count;
	uint32_t reserved_blocks;
	uint32_t free_blocks;
	uint32_t free_inodes;
	uint32_t first_data_block;
	uint32_t block_size;
	int32_t fragment_size;
	uint32_t blocks_per_group;
	uint32_t fragments_per_group;
	uint32_t inodes_per_group;
	uint32_t mount_time;
	uint32_t write_time;
	uint16_t mount_count;
	int16_t max_mount_count;
	uint16_t magic;
	uint16_t state;
	uint16_t errors;
	uint16_t minor_revision;
	uint32_t last_check_time;
	uint32_t check_interval;
	uint32_t creator_os;
	uint32_t revision;
	uint16_t default_res_uid;
	uint16_t default_res_gid;
	uint32_t first_inode;
	uint16_t inode_size;
	uint16_t blockgroup_num;
	uint32_t features_compat;
	uint32_t features_incompat;
	uint32_t features_ro;
	uint32_t volume_id[4];
	char volume_name[16];
	char last_mounted[64];
	uint32_t algo_bitmap;
	uint32_t reserved[205];
} __attribute__((packed)) ext2_superblock_t;

typedef struct ext2_blockgroup {
	uint32_t block_bitmap;
	uint32_t inode_bitmap;
	uint32_t inode_table;
	uint16_t free_blocks;
	uint16_t free_inodes;
	uint16_t used_directories;
	uint16_t padding;
	uint32_t reserved[3];
} __attribute__((packed)) ext2_blockgroup_t;

typedef struct ext2_inode {
	uint16_t mode;
	uint16_t uid;
	uint32_t size;
	uint32_t access_time;
	uint32_t creation_time;
	uint32_t modification_time;
	uint32_t deletion_time;
	uint16_t gid;
	uint16_t link_count;
	uint32_t block_count;
	uint32_t flags;
	uint32_t reserved1;
	uint32_t blocks[15];
	uint32_t version;
	uint32_t file_acl;
	uint32_t dir_acl;
	uint32_t fragment_address;
	uint8_t fragment_number;
	uint8_t fragment_size;
	uint16_t reserved2[5];
} __attribute__((packed)) ext2_inode_t;

typedef struct ext2_dirent {
	uint32_t inode;
	uint16_t record_len;
	uint8_t name_len;
	uint8_t type;
	char name[];
} __attribute__((packed)) ext2_dirent_t;

#define SUPERBLOCK_MAGIC 0xEF53
#define SUPERBLOCK_STATE_CLEAN 1
#define SUPERBLOCK_STATE_DIRTY 2
#define ROOT_INODE 2

// File Types
#define FT_IFSOCK	0xC000
#define FT_IFLNK	0xA000
#define FT_IFREG	0x8000
#define FT_IFBLK	0x6000
#define FT_IFDIR	0x4000
#define FT_IFCHR	0x2000
#define FT_IFIFO	0x1000

// Some helper macros for commonly needed conversions
#define inode_to_blockgroup(inode) ((inode - 1) / superblock->inodes_per_group)
#define mode_to_filetype(mode) (mode & 0xf000)
// TODO Should use right shift for negative values
#define superblock_to_blocksize(superblock) (1024 << superblock->block_size)

#define read_sector_or_fail(rc, args...) do {													\
		if(ide_read_sector(args) != true) {														\
			log(LOG_ERR, "ext2: IDE read failed in %s line %d, bailing.", __func__, __LINE__);	\
			return rc;																			\
		}																						\
	} while(0)

static ext2_superblock_t* superblock = NULL;


static char* filetype_to_verbose(int filetype) {
	switch(filetype) {
		case FT_IFSOCK: return "FT_IFSOCK";
		case FT_IFLNK: return "FT_IFLNK";
		case FT_IFREG: return "FT_IFREG";
		case FT_IFBLK: return "FT_IFBLK";
		case FT_IFDIR: return "FT_IFDIR";
		case FT_IFCHR: return "FT_IFCHR";
		case FT_IFIFO: return "FT_IFIFO";
		default: return NULL;
	}
}

static uint8_t* direct_read_blocks(uint32_t block_num, uint32_t read_num, uint8_t* buf) {
	debug("direct_read_blocks, reading block %d\n", block_num);

	// Allocate correct size + one HDD sector (Since we can only read in 512
	// byte chunks from the disk.
	if(!buf) {
		buf = (uint8_t*)kmalloc(superblock_to_blocksize(superblock) + 512);
	}

	block_num *= superblock_to_blocksize(superblock);
	block_num /= 512;
	read_num *= superblock_to_blocksize(superblock);
	read_num /= 512;

	for(int i = 0; i < read_num; i++) {
		read_sector_or_fail(NULL, 0x1F0, 0, block_num + i, buf + (i * 512));
	}

	return buf;
}

/* Reads an inode. Takes the number of the inode as argument and locates and
 * returns the corresponding ext2_inode_t*.
 */
static ext2_inode_t* read_inode(uint32_t inode_num)
{
	debug("Reading inode struct %d in blockgroup %d\n", inode_num, blockgroup_num);
	uint32_t blockgroup_num = inode_to_blockgroup(inode_num);

	// Sanity check the blockgroup num
	if(blockgroup_num > (superblock->block_count / superblock->blocks_per_group))
		return NULL;

	/* The number of blocks occupied by the blockgroup table
	 * There doesn't seem to be a way to directly get the number of blockgroups,
	 * so figure it out by dividing block count with blocks per group. Multiply
	 * with struct size to get total space required, then divide by block size
	 * to get ext2 blocks. Add 1 since partially used blocks also need to be
	 * allocated.
	 */
	uint32_t num_table_blocks = superblock->block_count
		/ superblock->blocks_per_group
		* sizeof(ext2_blockgroup_t)
		/ superblock_to_blocksize(superblock)
		+ 1;

	// FIXME Only read relevant offset
	ext2_blockgroup_t* blockgroup = kmalloc(superblock_to_blocksize(superblock) * num_table_blocks);
	if(!direct_read_blocks(2, num_table_blocks, (intptr_t)blockgroup)) {
		kfree(blockgroup);
		return NULL;
	}

	blockgroup += blockgroup_num;
	if(!blockgroup || !blockgroup->inode_table) {
		debug("Could not locate entry %d in blockgroup table\n", blockgroup_num);
		kfree(blockgroup);
		return NULL;
	}

	// Read inode table for this block group
	// TODO Only read the relevant parts (Or cache)
	uint8_t* inode = (uint8_t*)kmalloc(superblock->inodes_per_group * superblock->inode_size);
	uint32_t num_inode_blocks = superblock->inodes_per_group * superblock->inode_size / 1024;
	uint8_t* read = direct_read_blocks((intptr_t)blockgroup->inode_table, num_inode_blocks, inode);
	kfree(blockgroup);

	if(!read) {
		kfree(inode);
		return NULL;
	}

	inode += (inode_num - 1) % superblock->inodes_per_group * superblock->inode_size;
	return (ext2_inode_t*)inode;
}

static uint8_t* read_inode_block(ext2_inode_t* inode, uint32_t block_num, uint8_t* buf)
{
	if(block_num > superblock->block_count) {
		debug("read_inode_block: Invalid block_num (%d > %d)\n", block_num, superblock->block_count);
		return NULL;
	}

	uint32_t real_block_num = 0;

	// FIXME This value (268) actually depends on block size. This expects a 1024 block size
	if(block_num >= 12 && block_num < 268) {
		debug("reading indirect block at 0x%x\n", inode->blocks[12]);

		// Indirect block
		uint32_t* blocks_table = (uint32_t*)direct_read_blocks(inode->blocks[12], 1, NULL);
		real_block_num = blocks_table[block_num - 12];
		kfree(blocks_table);
	} else if(block_num >= 268 && block_num < 12 + 256*256) {
		uint32_t* blocks_table = (uint32_t*)direct_read_blocks(inode->blocks[13], 1, NULL);
		uint32_t indir_block_num = blocks_table[(block_num - 268) / 256];

		blocks_table = (uint32_t*)direct_read_blocks(indir_block_num, 1, NULL);
		real_block_num = blocks_table[(block_num - 268) % 256];
		kfree(blocks_table);
	} else {
		real_block_num = inode->blocks[block_num];
	}

	if(!real_block_num) {
		return NULL;
	}

	debug("read_inode_block: Translated inode block %d to real block %d\n", block_num, real_block_num);

	uint8_t* block = direct_read_blocks(real_block_num, 1, buf);
	if(!block) {
		return NULL;
	}

	return block;
}

// Returns the dirent for the n-th file in a directory inode
static ext2_dirent_t* read_dirent(uint32_t inode_num, uint32_t offset)
{
	ext2_inode_t* inode = read_inode(inode_num);
	if(!inode)
		return NULL;

	// Check if this inode is a directory
	if(mode_to_filetype(inode->mode) != FT_IFDIR)
	{
		log(LOG_WARN, "ext2_read_directory: This inode isn't a directory "
			"(Is %s [%d])\n", filetype_to_verbose(mode_to_filetype(inode->mode)),
				inode->mode);

		kfree(inode);
		return NULL;
	}

	// TODO
	uint8_t* block = read_inode_block(inode, 0, NULL);
	kfree(inode);

	if(!block) {
		return NULL;
	}

	block += 0x18; // WHY?

	// TODO Figure out how to find out the num of dirents in a block.
	// separate struct, NULL pointer?

	ext2_dirent_t* dirent = (ext2_dirent_t*)((intptr_t)block);

	// Get offset
	for(int i = 0; i < offset; i++)
		dirent = ((ext2_dirent_t*)((intptr_t)dirent + dirent->record_len));

	// This surely can't be right
	if(*((int*)dirent) == 0 || dirent->name == NULL || dirent->name_len == 0)
		return NULL;

	return dirent;
}

// Traverses directory tree to get inode from path
uint32_t inode_from_path(char* path)
{
	debug("Resolving inode for path %s\n", path);

	// The root directory always has inode 2
	if(unlikely(!strcmp("/", path)))
		return ROOT_INODE;

	// Split path and iterate trough the single parts, going from / upwards.
	char* pch;
	char* sp;

	// Throwaway pointer for strtok_r
	char* path_tmp = (char*)kmalloc((strlen(path) + 1) * sizeof(char));
	strcpy(path_tmp, path);

	pch = strtok_r(path_tmp, "/", &sp);
	uint32_t current_inode_num = ROOT_INODE;

	while(pch != NULL)
	{
		// Now search the current inode for the searched directory part
		// TODO Maybe use a binary search or something similar here
		for(int i = 0;; i++)
		{
			ext2_dirent_t* dirent = read_dirent(current_inode_num, i);

			// If this dirent is NULL, this means there are no more files
			if(!dirent)
				return 0;

			char* dirent_name = strndup(dirent->name, dirent->name_len);

			// Check if this is what we're searching for
			if(!strcmp(pch, dirent_name))
			{
				current_inode_num = dirent->inode;
				kfree(dirent_name);
				break;
			}

			kfree(dirent_name);
		}

		pch = strtok_r(NULL, "/", &sp);
	}

	kfree(path_tmp);

	debug("Inode for path %s is %d\n", path, current_inode_num);
	return current_inode_num;
}

// Reads multiple inode data blocks at once and create a continuous data stream
uint8_t* read_inode_blocks(ext2_inode_t* inode, uint32_t num, uint8_t* buf)
{
	debug("Reading %d inode blocks for ext2_inode_t* 0x%x\n", num, inode);
	debug("kmalloc = %d\n", superblock_to_blocksize(superblock) * num);

	for(int i = 0; i < num; i++)
	{
		uint8_t* current_block = read_inode_block(inode, i, buf + superblock_to_blocksize(superblock) * i);
		if(!current_block)
		{
			debug("read_inode_blocks: read_inode_block for block %d failed\n", i);
			return NULL;
		}
	}

	return buf;
}

#ifdef EXT2_DEBUG
char* ext2_get_verbose_permissions(ext2_inode_t* inode) {
	char* permstring = "         ";
	permstring[0] = (inode->mode & 0x0100) ? 'r' : '-';
	permstring[1] = (inode->mode & 0x0080) ? 'w' : '-';
	permstring[2] = (inode->mode & 0x0040) ? 'x' : '-';
	permstring[3] = (inode->mode & 0x0020) ? 'r' : '-';
	permstring[4] = (inode->mode & 0x0010) ? 'w' : '-';
	permstring[5] = (inode->mode & 0x0008) ? 'x' : '-';
	permstring[6] = (inode->mode & 0x0004) ? 'r' : '-';
	permstring[7] = (inode->mode & 0x0002) ? 'w' : '-';
	permstring[8] = (inode->mode & 0x0001) ? 'x' : '-';
	permstring[9] = 0;
	return permstring;
}
#endif

// The public readdir interface to the virtual file system
char* ext2_read_directory(char* path, uint32_t offset)
{
	uint32_t inode_num = inode_from_path(path);
	if(inode_num < 1)
		return NULL;

	ext2_dirent_t* dirent = read_dirent(inode_num, offset);
	if(!dirent)
		return NULL;

	return strndup(dirent->name, dirent->name_len);
}

// The public read interface to the virtual file system
size_t ext2_read_file(void* dest, uint32_t size, char* path, uint32_t offset)
{
	if(!path || !strcmp(path, "")) {
		log(LOG_ERR, "ext2: ext2_read_file called with empty path.\n");
		return NULL;
	}

	debug("ext2_read_file for %s, off %d, size %d\n", path, offset, size);

	uint32_t inode_num = inode_from_path(path);
	if(inode_num < 1)
		return NULL;

	ext2_inode_t* inode = read_inode(inode_num);
	if(!inode)
		return NULL;

	debug("%s found at ext2_inode_t* 0x%x\n", path, inode);
	debug("%s uid=%d, gid=%d, size=%d, ft=%s mode=%s\n", path, inode->uid,
		inode->gid, inode->size, filetype_to_verbose(mode_to_filetype(inode->mode)),
		ext2_get_verbose_permissions(inode));

	uint32_t file_type = mode_to_filetype(inode->mode);

	if(file_type == FT_IFLNK) {
		/* For symlinks with up to 60 chars length, the path is stored in the
		 * inode in the area where normally the block pointers would be.
		 * Otherwise in the file itself.
		 */
		if(inode->size > 60) {
			log(LOG_WARN, "ext2: Symlinks with length >60 are not supported right now.\n");
			kfree(inode);
			return NULL;
		}

		char* sym_path = (char*)inode->blocks;
		if(sym_path[0] != '/') {
			log(LOG_WARN, "ext2: Relative symlinks not supported right now.\n");
			kfree(inode);
			return NULL;
		}

		size_t r = ext2_read_file(dest, size, sym_path, offset);
		kfree(inode);
		return r;
	}

	if(file_type != FT_IFREG)
	{
		log(LOG_WARN, "ext2_read_file: Attempt to read something weird "
			"(0x%x: %s)\n", inode->mode,
			filetype_to_verbose(mode_to_filetype(inode->mode)));

		kfree(inode);
		return NULL;
	}

	if(inode->size < 1) {
		kfree(inode);
		return NULL;
	}

	if(size > inode->size) {
		debug("ext2_read_file: Attempt to read 0x%x bytes, but file is only 0x%x bytes. Capping.\n", size, inode->size);
		size = inode->size;
	}

	uint32_t num_blocks = (size + offset) / superblock_to_blocksize(superblock);
	if((size + offset) % superblock_to_blocksize(superblock) != 0) {
		num_blocks++;
	}

	debug("Inode has %d blocks:\n", num_blocks);
	debug("Blocks table:\n");
	for(uint32_t i = 0; i < 15; i++) {
		debug("\t%d: 0x%x\n", i, inode->blocks[i]);
	}

	uint8_t* read = read_inode_blocks(inode, num_blocks, dest);
	kfree(inode);

	if(!read) {
		return NULL;
	}

	#ifdef EXT2_DEBUG
		printf("Read file %s offset %d size %d with resulting md5sum of:\n\t", path, offset, size);
		MD5_dump(dest, size);
	#endif

	return size;
}

void ext2_init()
{
	// The superblock always has an offset of 1024, so is in sector 2 & 3
	superblock = (ext2_superblock_t*)kmalloc(1024);
	read_sector_or_fail(, 0x1F0, 0, 2, (uint8_t*)superblock);
	read_sector_or_fail(, 0x1F0, 0, 3, (uint8_t*)((void*)superblock + 512));

	if(superblock->magic != SUPERBLOCK_MAGIC)
	{
		log(LOG_ERR, "ext2: Invalid magic\n");
		return;
	}

	log(LOG_INFO, "ext2: Have ext2 revision %d. %d free / %d blocks.\n",
			superblock->revision, superblock->free_blocks,
			superblock->block_count);


	// Check if the file system is marked as clean
	if(superblock->state != SUPERBLOCK_STATE_CLEAN)
	{
		log(LOG_ERR, "ext2: File system is not marked as clean.\n"
			"Please run fsck.ext2 on it.\n");
		return;
	}

	/*if(!superblock_to_blocksize(superblock) != 1024) {
		log(LOG_ERR, "ext2: Block sizes != 1024 are not supported right now.\n");
		return;
	}*/

	// TODO Compare superblocks to each other?

	// RO is irrelevant for now since we're read-only anyways.
	//if(superblock->features_incompat || superblock->features_ro)
	if(superblock->features_incompat)
	{
		log(LOG_ERR, "ext2: Sorry, but this filesystem uses some extensions "
			"which I don't support (incompat: 0x%x, ro: 0x%x)\n",
			superblock->features_incompat, superblock->features_ro);
		//return;
	}

	if(superblock->features_compat)
	{
		log(LOG_INFO, "ext2: This file system supports additional special "
			"features. I'll ignore them (0x%x).\n", superblock->features_compat);
	}

	debug("Loaded ext2 superblock. inode_count=%d, block_count=%d, block_size=%d\n",
		superblock->inode_count, superblock->block_count, superblock_to_blocksize(superblock));

	vfs_mount("/", NULL, ext2_read_file, ext2_read_directory);
}

#endif /* ENABLE_EXT2 */
