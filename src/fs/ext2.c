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

#include <log.h>
#include <string.h>
#include <errno.h>
#include <md5.h>
#include <memory/kmalloc.h>
#include <hw/ide.h>
#include <hw/serial.h>
#include <fs/vfs.h>
#include "ext2.h"

#ifdef EXT2_DEBUG
  #define debug(args...) log(LOG_DEBUG, "ext2: " args)
#else
  #define debug(...)
#endif

struct superblock {
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
} __attribute__((packed));

struct blockgroup {
	uint32_t block_bitmap;
	uint32_t inode_bitmap;
	uint32_t inode_table;
	uint16_t free_blocks;
	uint16_t free_inodes;
	uint16_t used_directories;
	uint16_t padding;
	uint32_t reserved[3];
} __attribute__((packed));

struct inode {
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
} __attribute__((packed));

#define SUPERBLOCK_MAGIC 0xEF53
#define SUPERBLOCK_STATE_CLEAN 1
#define SUPERBLOCK_STATE_DIRTY 2
#define ROOT_INODE 2

#define inode_to_blockgroup(inode) ((inode - 1) / superblock->inodes_per_group)
// TODO Should use right shift for negative values
#define superblock_to_blocksize(superblock) (1024 << superblock->block_size)

#define read_sector_or_fail(rc, args...) do {													\
		if(ide_read_sector(args) != true) {														\
			log(LOG_ERR, "ext2: IDE read failed in %s line %d, bailing.", __func__, __LINE__);	\
			return rc;																			\
		}																						\
	} while(0)

static struct superblock* superblock = NULL;
struct blockgroup* blockgroup_table = NULL;
struct inode* root_inode = NULL;

static uint8_t* direct_read_blocks(uint32_t block_num, uint32_t read_num, uint8_t* buf) {
	debug("direct_read_blocks, reading block %d\n", block_num);

	// Allocate correct size + one HDD sector (Since we can only read in 512
	// byte chunks from the disk.
	if(!buf) {
		buf = (uint8_t*)kmalloc(superblock_to_blocksize(superblock) * read_num);
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

static bool read_inode(struct inode* buf, uint32_t inode_num)
{
	if(inode_num == ROOT_INODE && root_inode) {
		memcpy(buf, root_inode, superblock->inode_size);
		return true;
	}

	uint32_t blockgroup_num = inode_to_blockgroup(inode_num);
	debug("Reading inode struct %d in blockgroup %d\n", inode_num, blockgroup_num);

	// Sanity check the blockgroup num
	if(blockgroup_num > (superblock->block_count / superblock->blocks_per_group))
		return false;

	struct blockgroup* blockgroup = blockgroup_table + blockgroup_num;
	if(!blockgroup || !blockgroup->inode_table) {
		debug("Could not locate entry %d in blockgroup table\n", blockgroup_num);
		return false;
	}

	// Read inode table for this block group
	// TODO Only read the relevant parts (Or cache)
	uint8_t* table = (uint8_t*)kmalloc(superblock->inodes_per_group * superblock->inode_size);
	uint32_t num_inode_blocks = superblock->inodes_per_group * superblock->inode_size / 1024;
	uint8_t* read = direct_read_blocks((intptr_t)blockgroup->inode_table, num_inode_blocks, table);

	if(!read) {
		kfree(table);
		return false;
	}

	memcpy(buf, table + ((inode_num - 1) % superblock->inodes_per_group * superblock->inode_size), superblock->inode_size);
	kfree(table);
	return true;
}


static uint8_t* read_inode_block(struct inode* inode, uint32_t block_num, uint8_t* buf)
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

		uint32_t* indir_blocks_table = (uint32_t*)direct_read_blocks(indir_block_num, 1, NULL);
		real_block_num = indir_blocks_table[(block_num - 268) % 256];
		kfree(blocks_table);
		kfree(indir_blocks_table);
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

// Reads multiple inode data blocks at once and create a continuous data stream
uint8_t* read_inode_blocks(struct inode* inode, uint32_t num, uint8_t* buf)
{
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

// The public open interface to the virtual file system
uint32_t ext2_open(char* path, void* mount_instance) {
	if(!path || !strcmp(path, "")) {
		log(LOG_ERR, "ext2: ext2_read_file called with empty path.\n");
		return 0;
	}

	debug("Resolving inode for path %s\n", path);

	// The root directory always has inode 2
	if(unlikely(!strcmp("/", path)))
		return ROOT_INODE;

	// Split path and iterate trough the single parts, going from / upwards.
	char* pch;
	char* sp;

	// Throwaway pointer for strtok_r
	char* path_tmp = strndup(path, 500);
	pch = strtok_r(path_tmp, "/", &sp);
	struct inode* current_inode = kmalloc(superblock->inode_size);
	vfs_dirent_t* dirent = NULL;
	uint8_t* dirent_block = NULL;

	while(pch != NULL)
	{
		if(!read_inode(current_inode, dirent ? dirent->inode : ROOT_INODE)) {
			continue;
		}

		if(dirent_block) {
			kfree(dirent_block);
		}

		dirent_block = kmalloc(current_inode->size);
		if(!read_inode_blocks(current_inode, current_inode->size / superblock_to_blocksize(superblock), dirent_block)) {
			kfree(dirent_block);
			kfree(path_tmp);
			kfree(current_inode);
			return 0;
		}

		dirent = (vfs_dirent_t*)dirent_block;

		// Now search the current inode for the searched directory part
		// TODO Maybe use a binary search or something similar here
		for(int i = 0;; i++)
		{
			// If this dirent is NULL, this means there are no more files
			if(!dirent || !dirent->name_len) {
				kfree(dirent_block);
				kfree(path_tmp);
				kfree(current_inode);
				return 0;
			}

			char* dirent_name = strndup(dirent->name, dirent->name_len);

			// Check if this is what we're searching for
			if(!strcmp(pch, dirent_name))
			{
				kfree(dirent_name);
				break;
			}

			kfree(dirent_name);
			dirent = ((vfs_dirent_t*)((intptr_t)dirent + dirent->record_len));
		}

		pch = strtok_r(NULL, "/", &sp);
	}

	uint32_t inode_num = dirent->inode;
	kfree(path_tmp);
	kfree(dirent_block);
	kfree(current_inode);

	// Handle symbolic links
	struct inode* inode = kmalloc(superblock->inode_size);
	if(!read_inode(inode, inode_num)) {
		return 0;
	}

	if(vfs_mode_to_filetype(inode->mode) == FT_IFLNK) {
		/* For symlinks with up to 60 chars length, the path is stored in the
		 * inode in the area where normally the block pointers would be.
		 * Otherwise in the file itself.
		 */
		if(inode->size > 60) {
			log(LOG_WARN, "ext2: Symlinks with length >60 are not supported right now.\n");
			kfree(inode);
			return 0;
		}

		char* sym_path = (char*)inode->blocks;
		char* new_path;
		if(sym_path[0] != '/') {
			char* base_path = strdup(path);
			char* c = base_path + strlen(path);
			for(; c > path; c--) {
				if(*c == '/') {
					*c = 0;
					break;
				}
			}

			new_path = vfs_normalize_path(sym_path, base_path);
			kfree(base_path);
		} else {
			new_path = strdup(sym_path);
		}

		kfree(inode);

		// FIXME Should be vfs_open to make symlinks across mount points possible
		uint32_t r = ext2_open(new_path, mount_instance);
		kfree(new_path);
		return r;
	}

	kfree(inode);
	return inode_num;
}

// The public read interface to the virtual file system
size_t ext2_read_file(vfs_file_t* fp, void* dest, size_t size)
{
	if(!fp || !fp->inode) {
		log(LOG_ERR, "ext2: ext2_read_file called without fp or fp missing inode.\n");
		sc_errno = EBADF;
		return -1;
	}

	debug("ext2_read_file for %s, off %d, size %d\n", fp->mount_path, fp->offset, size);

	struct inode* inode = kmalloc(superblock->inode_size);
	if(!read_inode(inode, fp->inode)) {
		kfree(inode);
		sc_errno = EBADF;
		return -1;
	}

	debug("%s uid=%d, gid=%d, size=%d, ft=%s mode=%s\n", fp->mount_path, inode->uid,
		inode->gid, inode->size, vfs_filetype_to_verbose(vfs_mode_to_filetype(inode->mode)),
		vfs_get_verbose_permissions(inode->mode));

	if(vfs_mode_to_filetype(inode->mode) != FT_IFREG)
	{
		debug("ext2_read_file: Attempt to read something weird "
			"(0x%x: %s)\n", inode->mode,
			vfs_filetype_to_verbose(vfs_mode_to_filetype(inode->mode)));

		kfree(inode);
		sc_errno = EISDIR;
		return -1;
	}

	if(inode->size < 1) {
		kfree(inode);
		return 0;
	}

	if(size > inode->size) {
		debug("ext2_read_file: Attempt to read 0x%x bytes, but file is only 0x%x bytes. Capping.\n", size, inode->size);
		size = inode->size;
	}

	uint32_t num_blocks = (size + fp->offset) / superblock_to_blocksize(superblock);
	if((size + fp->offset) % superblock_to_blocksize(superblock) != 0) {
		num_blocks++;
	}

	debug("Inode has %d blocks:\n", num_blocks);
	debug("Blocks table:\n");
	for(uint32_t i = 0; i < 15; i++) {
		debug("\t%d: 0x%x\n", i, inode->blocks[i]);
	}

	/* This should copy directly to dest, however read_inode_blocks can only read
	 * whole blocks right now, which means we could write more than size if size
	 * is not mod the block size. Should rewrite read_inode_blocks.
	 */
	uint8_t* tmp = kmalloc(num_blocks * superblock_to_blocksize(superblock));
	uint8_t* read = read_inode_blocks(inode, num_blocks, tmp);
	kfree(inode);

	if(!read) {
		kfree(tmp);
		return 0;
	}

	memcpy(dest, tmp, size);
	kfree(tmp);

	#ifdef EXT2_DEBUG
		printf("Read file %s offset %d size %d with resulting md5sum of:\n\t", fp->mount_path, fp->offset, size);
		MD5_dump(dest, size);
	#endif

	return size;
}

size_t ext2_getdents(vfs_file_t* fp, void* dest, size_t size) {
	if(size % 1024) {
		log(LOG_ERR, "ext2: Size argument to ext2_getdents needs to be a multiple of 1024.\n");
		return 0;
	}

	if(!fp || !fp->inode) {
		log(LOG_ERR, "ext2: ext2_read_directory called without fp or fp missing inode.\n");
		return 0;
	}

	struct inode* inode = kmalloc(superblock->inode_size);
	if(!read_inode(inode, fp->inode)) {
		kfree(inode);
		return 0;
	}

	// Check if this inode is a directory
	if(vfs_mode_to_filetype(inode->mode) != FT_IFDIR)
	{
		debug("ext2_read_directory: This inode isn't a directory "
			"(Is %s [%d])\n", vfs_filetype_to_verbose(vfs_mode_to_filetype(inode->mode)),
				inode->mode);

		kfree(inode);
		return 0;
	}

	if(!read_inode_blocks(inode, size / superblock_to_blocksize(superblock), dest)) {
		return 0;
	}

	kfree(inode);
	return 1;
}

int ext2_stat(vfs_file_t* fp, vfs_stat_t* dest) {
	if(!fp || !fp->inode) {
		log(LOG_ERR, "ext2: ext2_read_file called without fp or fp missing inode.\n");
		return -1;
	}

	struct inode* inode = kmalloc(superblock->inode_size);
	if(!read_inode(inode, fp->inode)) {
		kfree(inode);
		return -1;
	}

	dest->st_dev = 1;
	dest->st_ino = fp->inode;
	dest->st_mode = inode->mode;
	dest->st_nlink = 0;
	dest->st_uid = inode->uid;
	dest->st_gid = inode->gid;
	dest->st_rdev = 0;
	dest->st_size = inode->size;
	dest->st_atime = inode->access_time;
	dest->st_mtime = inode->modification_time;
	dest->st_ctime = inode->creation_time;

	kfree(inode);
	return 0;
}

void ext2_init()
{
	// The superblock always has an offset of 1024, so is in sector 2 & 3
	superblock = (struct superblock*)kmalloc(1024);
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
		log(LOG_WARN, "ext2: This filesystem uses some extensions "
			"which we don't support (incompat: 0x%x, ro: 0x%x)\n",
			superblock->features_incompat, superblock->features_ro);
		//return;
	}

	if(superblock->features_compat)
	{
		log(LOG_INFO, "ext2: This file system supports additional special "
			"features. We'll ignore them (0x%x).\n", superblock->features_compat);
	}

	debug("Loaded ext2 superblock. inode_count=%d, block_count=%d, block_size=%d\n",
		superblock->inode_count, superblock->block_count, superblock_to_blocksize(superblock));

	/* The number of blocks occupied by the blockgroup table
	 * There doesn't seem to be a way to directly get the number of blockgroups,
	 * so figure it out by dividing block count with blocks per group. Multiply
	 * with struct size to get total space required, then divide by block size
	 * to get ext2 blocks. Add 1 since partially used blocks also need to be
	 * allocated.
	 */
	uint32_t num_table_blocks = superblock->block_count
		/ superblock->blocks_per_group
		* sizeof(struct blockgroup)
		/ superblock_to_blocksize(superblock)
		+ 1;

	blockgroup_table = kmalloc(superblock_to_blocksize(superblock) * num_table_blocks);
	if(!direct_read_blocks(2, num_table_blocks, (uint8_t*)blockgroup_table)) {
		kfree(superblock);
		kfree(blockgroup_table);
		return;
	}

	// Cache root inode
	struct inode* root_inode_buf = kmalloc(superblock->inode_size);
	if(!read_inode(root_inode_buf, ROOT_INODE)) {
		log(LOG_ERR, "ext2: Could not read root inode.\n");
		kfree(superblock);
		kfree(root_inode_buf);
		kfree(blockgroup_table);
		return;
	}

	root_inode = root_inode_buf;
	vfs_mount("/", NULL, "/dev/ide1", "ext2", ext2_open, ext2_stat, ext2_read_file, ext2_getdents);
}

#endif /* ENABLE_EXT2 */
