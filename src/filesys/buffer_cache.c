#include "filesys/buffer_cache.h"
#include "devices/block.h"
#include <stdbool.h>
#include <debug.h>

#define BUFFER_CACHE_ENTRY_NB 64    // number of buffer cache entry, 64*512Byte=32KByte

char *p_buffer_cache;               // points buffer cache memory
struct buffer_head buffer_head[BUFFER_CACHE_ENTRY_NB];
                                       // array of buffer head
int clock_hand;  // variable for clock algorithm when choosing victim
struct lock lock;

bool bc_read(block_sector_t sector_idx, void *buffer,
			 off_t bytes_read, int chunk_size, int sector_ofs){
	struct buffer_head *bh = bc_lookup(sector_idx);
	if(!bh){
		bh = bc_select_victim();
		bc_flush_entry(bh);
		bh->is_used = true;
		bh->dirty = false;
		bh->sector = sector_idx;
		//lock_release(&lock);
		block_read(fs_device, sector_idx, bh->data);
	}
	memcpy(buffer + bytes_read, bh->data + sector_ofs, chunk_size);
	bh->clock = true;
	//lock_release(&bh->lock);

	return true;
}

bool bc_write(block_sector_t sector_idx, void *buffer,
			 off_t bytes_written, int chunk_size, int sector_ofs){
	struct buffer_head *bh = bc_lookup(sector_idx);
	if(!bh){
		bh = bc_select_victim();
		bc_flush_entry(bh);
		bh->is_used = true;
		bh->sector = sector_idx;
		//lock_release(&lock);
		block_read(fs_device, sector_idx, bh->data);
	}
	memcpy(bh->data + sector_ofs, buffer + bytes_written,chunk_size);
	bh->clock = true;
	bh->dirty = true;
	//lock_release(&bh->lock);

	return true;
}

void bc_init(void){
	p_buffer_cache = (char*)malloc(BLOCK_SECTOR_SIZE*BUFFER_CACHE_ENTRY_NB);
	struct buffer_head *bh;
	void *bc = p_buffer_cache;
	for(bh = buffer_head; bh < buffer_head + BUFFER_CACHE_ENTRY_NB; bh++){
		memset(bh, 0, sizeof(struct buffer_head));
		lock_init(&bh->lock);
		bh->data = bc;
		bc += BLOCK_SECTOR_SIZE;
	}
	clock_hand = 0;
	lock_init(&lock);
}

void bc_term(void){
	bc_flush_all_entries();
	free(p_buffer_cache);
}

struct buffer_head* bc_select_victim(void){
	int origin = clock_hand;
	for(clock_hand; clock_hand < BUFFER_CACHE_ENTRY_NB; clock_hand++){
		struct buffer_head *bh = &buffer_head[clock_hand];
		//lock_acquire(&bh->lock);
		if(!bh->clock || !bh->is_used){
			clock_hand++;
			return bh;
		}
		bh->clock = false;
		//lock_release(&bh->lock);
	}
	clock_hand = 0;
	for(clock_hand; clock_hand < origin; clock_hand++){
		struct buffer_head *bh = &buffer_head[clock_hand];
		//lock_acquire(&bh->lock);
		if(!bh->clock || !bh->is_used){
			clock_hand++;
			return bh;
		}
		bh->clock = false;
		//lock_release(&bh->lock);
	}
}

struct buffer_head* bc_lookup(block_sector_t sector){
	//lock_acquire(&lock);
	struct buffer_head *bh;
	for(bh = buffer_head; bh < buffer_head + BUFFER_CACHE_ENTRY_NB; bh++){
		if(bh->is_used && bh->sector == sector){
			//lock_acquire(&bh->lock);
			//lock_release(&lock);
			return bh;
		}
	}
	return NULL;
}

void bc_flush_entry(struct buffer_head *p_flush_entry){
	if(!p_flush_entry->is_used || !p_flush_entry->dirty)
		return;
	p_flush_entry->dirty = false;
	block_write(fs_device, p_flush_entry->sector, p_flush_entry->data);
}

void bc_flush_all_entries(void){
	struct buffer_head *bh;
	for(bh = buffer_head; bh < buffer_head + BUFFER_CACHE_ENTRY_NB; bh++){
		//lock_acquire(&bh->lock);
		bc_flush_entry(bh);
		//lock_release(&bh->lock);
	}
}
