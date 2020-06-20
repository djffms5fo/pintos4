#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/synch.h"
#include "devices/block.h"
#include "filesys/off_t.h"
#include <stdbool.h>

struct buffer_head{
	bool dirty;           // flag that entry is dirty or not
	bool is_used;            // flag that entry is used or not
	block_sector_t sector;     // disk sector address of entry
	bool clock;              // clock bit for clock algorithm
	struct lock lock;        // lock variable
	void* data;               // pointer that points buffer cache entry
};

bool bc_read(block_sector_t sector_idx, void *buffer,
			 off_t bytes_read, int chunk_size, int sector_ofs);
bool bc_write(block_sector_t sector_idx, void *buffer, 
			  off_t bytes_written, int chunk_size, int sector_ofs);
void bc_init(void);
void bc_term(void);
struct buffer_head* bc_select_victim(void);
struct buffer_head* bc_lookup(block_sector_t sector);
void bc_flush_entry(struct buffer_head *p_flush_entry);
void bc_flush_all_entries(void);

#endif