#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// number of next index block that one index block can restore
#define INDIRECT_BLOCK_ENTIRES (BLOCK_SECTOR_SIZE / sizeof(block_sector_t))

// number of block that will be restored at inode directly 
// declare size of inode_disk to be one block size(512Byte)
#define DIRECT_BLOCK_ENTRIES 123

// type of table
enum direct_t{
  NORMAL_DIRECT,    // restore disk block number into inode
  INDIRECT,         // access disk block number with one index block
  DOUBLE_INDIRECT,  // access disk block number with two index block
  OUT_LIMIT         // offset is invalid
};

// structure for table type and two indices
struct sector_location{
  int directness;   // disk block accessing method from enum direct_t
  int index1;       // offset of entry that first index block will access
  int index2;       // offset of entry that second index block will access
};

// indicates sector number, size is BLOCK_SECTOR_SIZE
struct inode_indirect_block{
  block_sector_t map_table[INDIRECT_BLOCK_ENTIRES]; // array of block sector number
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    //block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    //uint32_t unused[125];               /* Not used. */
    uint32_t is_dir;
    // array of disk block number that will be accessed directly
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    // block number that will be accessed indirectly 
    block_sector_t indirect_block_sec;
    // first index block when accessed by double indirect method
    block_sector_t double_indirect_block_sec;

  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

static inline off_t map_table_offset(int index){
  return 4*index;
}

static bool get_disk_inode(const struct inode *inode, struct inode_disk *inode_disk);
static void locate_byte(off_t pos, struct sector_location *sec_loc);
static bool register_sector(struct inode_disk *inode_disk, block_sector_t new_sector,
                            struct sector_location sec_loc);
static void free_inode_sectors(struct inode_disk *inode_disk);
static bool inode_update_file_length(struct inode_disk* inode_disk,
                                off_t start_pos, off_t end_pos);

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock extend_lock;            // Semaphore lock
    //struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos) 
{
  ASSERT (inode_disk != NULL);

  block_sector_t result_sec;

  if (pos < inode_disk->length){
    struct inode_indirect_block *ind_block;
    struct sector_location sec_loc;
    locate_byte(pos, &sec_loc);
    switch(sec_loc.directness){
      case NORMAL_DIRECT:
        result_sec = inode_disk->direct_map_table[sec_loc.index1];
        break;
      case INDIRECT:
        if(inode_disk->indirect_block_sec == (block_sector_t) -1){
          return -1;
        }
        ind_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(ind_block){
          bc_read(inode_disk->indirect_block_sec, ind_block, 0,
                  sizeof(struct inode_indirect_block), 0);
          result_sec = ind_block->map_table[sec_loc.index1];
        }
        else
          result_sec = 0;
        free(ind_block);
        break;
      case DOUBLE_INDIRECT:
        if(inode_disk->double_indirect_block_sec == (block_sector_t) -1)
          return -1;
        ind_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(ind_block){
          bc_read(inode_disk->double_indirect_block_sec, ind_block, 0,
                  sizeof(struct inode_indirect_block), 0);
          result_sec = ind_block->map_table[sec_loc.index2];
          if(result_sec == (block_sector_t) -1)
            return -1;
          bc_read(result_sec, &ind_block, 0, sizeof(struct inode_indirect_block), 0);
          result_sec = ind_block->map_table[sec_loc.index1];
        }
        else
          result_sec = 0;
        free(ind_block);
        break;
      default:
        return -1;
    }
    return result_sec;
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      //size_t sectors = bytes_to_sectors (length);
      memset(disk_inode, -1, sizeof(struct inode_disk));
      disk_inode->length = length;
      if(length > 0){
        inode_update_file_length(disk_inode, 0, length);
      }
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      
      /*if (free_map_allocate (sectors, &disk_inode->start)) 
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                block_write (fs_device, disk_inode->start + i, zeros);
            }
          success = true; 
        } */
      bc_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
      free (disk_inode);
      success = true;
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->extend_lock);
  //block_read (fs_device, inode->sector, &inode->data);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          struct inode_disk *disk_inode = (struct inode_disk*)malloc(BLOCK_SECTOR_SIZE);
          if(!disk_inode)
            return;
          bc_read(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
          //get_disk_inode(inode, disk_inode);
          free_inode_sectors(disk_inode);
          free_map_release(inode->sector, 1);
          free(disk_inode);
          /*free_map_release (inode->sector, 1);
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); */
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;
  struct inode_disk *disk_inode = (struct inode_disk*)malloc(BLOCK_SECTOR_SIZE);
  if(!disk_inode)
    return 0;

  lock_acquire(&inode->extend_lock);

  get_disk_inode(inode, disk_inode);


  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      if(sector_idx == (block_sector_t) -1)
        break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      lock_release(&inode->extend_lock);

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0){
        lock_acquire(&inode->extend_lock);
        break;
      }

      bc_read(sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
      lock_acquire(&inode->extend_lock);
    }
  lock_release(&inode->extend_lock);
  //free (bounce);
  free(disk_inode);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  struct inode_disk *disk_inode = (struct inode_disk*)malloc(BLOCK_SECTOR_SIZE);
  if(!disk_inode)
    return 0;

  lock_acquire(&inode->extend_lock);

  get_disk_inode(inode, disk_inode);

  int old_length = disk_inode->length;
  int write_end = offset + size - 1;

  if(write_end > old_length - 1){
    inode_update_file_length(disk_inode, disk_inode->length, offset + size);
    bc_write(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
  }

  //lock_release(&inode->extend_lock);  

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      lock_release(&inode->extend_lock);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0){
        lock_acquire(&inode->extend_lock);
        break;
      }

      bc_write(sector_idx, (void*)buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      lock_acquire(&inode->extend_lock);
    }
  //free (bounce);
  lock_release(&inode->extend_lock);
  free(disk_inode);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk inode_disk;
  bc_read(inode->sector, &inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  return inode_disk.length;
}

static bool
get_disk_inode(const struct inode *inode, struct inode_disk *inode_disk){
  bc_read(inode->sector, inode_disk, 0, sizeof(struct inode_disk), 0);
  return true;
}

static void
locate_byte(off_t pos, struct sector_location *sec_loc){
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;
  if(pos_sector < DIRECT_BLOCK_ENTRIES){
    sec_loc->index1 = pos_sector;
    sec_loc->directness = NORMAL_DIRECT;
  }
  else if(pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTIRES)){
    pos_sector -= DIRECT_BLOCK_ENTRIES;
    sec_loc->index1 = pos_sector;
    sec_loc->directness = INDIRECT;
  }
  else if(pos_sector < (off_t)(DIRECT_BLOCK_ENTRIES + 
                               INDIRECT_BLOCK_ENTIRES * (INDIRECT_BLOCK_ENTIRES + 1))){
    pos_sector -= DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTIRES;
    sec_loc->index2 = pos_sector / INDIRECT_BLOCK_ENTIRES;
    sec_loc->index1 = pos_sector % INDIRECT_BLOCK_ENTIRES;
    sec_loc->directness = DOUBLE_INDIRECT;
  }
  else
    sec_loc->directness = OUT_LIMIT;

}

static bool
register_sector(struct inode_disk *inode_disk, block_sector_t new_sector,
                struct sector_location sec_loc){
  struct inode_indirect_block *new_block, *new_block2;
  int rewrite = 0;

  switch(sec_loc.directness){
    case NORMAL_DIRECT:
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
      return true;

    case INDIRECT:
      if(inode_disk->indirect_block_sec == (block_sector_t) -1){
        if(!free_map_allocate(1, &inode_disk->indirect_block_sec))
          return false;
        new_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(!new_block)
          return false;        
        memset(new_block, -1, sizeof(struct inode_indirect_block));
      }
      else{
        new_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(!new_block)
          return false;   
        bc_read(inode_disk->indirect_block_sec, new_block, 0,
                sizeof(struct inode_indirect_block), 0);
      }
      if(new_block->map_table[sec_loc.index1] == (block_sector_t) -1)
        new_block->map_table[sec_loc.index1] = new_sector;
      bc_write(inode_disk->indirect_block_sec, new_block, 0,
               sizeof(struct inode_indirect_block), 0);
      break;

    case DOUBLE_INDIRECT:    
      if(inode_disk->double_indirect_block_sec == (block_sector_t) -1){
        if(!free_map_allocate(1, &inode_disk->double_indirect_block_sec))
          return false;
        new_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(!new_block)
          return false;
        memset(new_block, -1, sizeof(struct inode_indirect_block));
      }
      else{
        new_block = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
        if(!new_block)
          return false;
        bc_read(inode_disk->double_indirect_block_sec, new_block, 0,
                sizeof(struct inode_indirect_block), 0);
      }
      new_block2 = (struct inode_indirect_block*)malloc(BLOCK_SECTOR_SIZE);
      if(!new_block2)
        return false;
      if(new_block->map_table[sec_loc.index2]){
        rewrite = 1;
        if(!free_map_allocate(1, new_block2->map_table[sec_loc.index2]))
          return false;
        memset(new_block2, -1, sizeof(struct inode_indirect_block));
      }
      else
        bc_read(new_block->map_table[sec_loc.index2], new_block2, 0,
                sizeof(struct inode_indirect_block), 0);
      if(new_block2->map_table[sec_loc.index1] == (block_sector_t) -1)
        new_block2->map_table[sec_loc.index1] = new_sector;

      if(rewrite)
        bc_write(inode_disk->double_indirect_block_sec, new_block, 0,
                 sizeof(struct inode_indirect_block), 0);
      bc_write(inode_disk->double_indirect_block_sec, new_block2, 0,
               sizeof(struct inode_indirect_block), 0);
      break;
    default:
      return false;
  }
  free(new_block);
  free(new_block2);
  return true;
}

static bool inode_update_file_length(struct inode_disk* inode_disk,
                                     off_t start_pos, off_t end_pos){


  static char zeros[BLOCK_SECTOR_SIZE];
  memset(zeros, 0, BLOCK_SECTOR_SIZE);

  if(start_pos == end_pos)
    return true;
  if(start_pos > end_pos)
    return false;

  inode_disk->length = end_pos;
  end_pos--;

  start_pos = start_pos / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;
  end_pos = end_pos / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE;

  for( ; start_pos <= end_pos; start_pos += BLOCK_SECTOR_SIZE){
    struct sector_location sec_loc;
    block_sector_t sector = byte_to_sector(inode_disk, start_pos);
    if(sector != (block_sector_t) -1)
      continue;

    if(!free_map_allocate(1, &sector))
      return false;
    locate_byte(start_pos, &sec_loc);
    if(!register_sector(inode_disk, sector, sec_loc)){
      return false;
    }
    bc_write(sector, zeros, 0, BLOCK_SECTOR_SIZE, 0);
  }
  return true;




  /*char *zeros = (char*)malloc(BLOCK_SECTOR_SIZE);

  if(start_pos == end_pos)
    return true;
  if(start_pos > end_pos)
    return false;

  int size = end_pos - start_pos;
  int chunk_size;

  while(size > 0){
    struct sector_location sec_loc;
    int sector_ofs = start_pos % BLOCK_SECTOR_SIZE;
    block_sector_t sector_idx = byte_to_sector(inode_disk, start_pos - sector_ofs);
    if(sector_ofs > 0){
      chunk_size = BLOCK_SECTOR_SIZE - sector_ofs;
    }
    else{
      if(free_map_allocate(1, &sector_idx)){
        locate_byte(start_pos, &sec_loc);
        register_sector(inode_disk, sector_idx, sec_loc);
        chunk_size = BLOCK_SECTOR_SIZE;
      }
      else{
        free(zeros);
        return false;
      }
      bc_write(sector_idx, zeros, 0, BLOCK_SECTOR_SIZE, 0);
    }
    size -= chunk_size;
    start_pos += chunk_size;
  }
  free(zeros);
  return true;*/
}

static void free_inode_sectors(struct inode_disk *inode_disk){
  int i, j;

  if(inode_disk->double_indirect_block_sec > 0){
    i = 0;
    struct inode_indirect_block *ind_block_1 = malloc(BLOCK_SECTOR_SIZE);
    bc_read(inode_disk->indirect_block_sec, ind_block_1, 0,
            sizeof(struct inode_indirect_block), 0);
    while(ind_block_1->map_table[i] > 0){
      struct inode_indirect_block *ind_block_2 = malloc(BLOCK_SECTOR_SIZE);
      bc_read(ind_block_1->map_table[i], ind_block_2, 0,
              sizeof(struct inode_indirect_block), 0);
      j = 0;
      while(ind_block_2->map_table[j] > 0){
        free_map_release(ind_block_2->map_table[j], 1);
        j++;
      }
      free_map_release(ind_block_1->map_table[i], 1);
      i++;
      free(ind_block_2);
    }
    free_map_release(inode_disk->double_indirect_block_sec, 1);
    free(ind_block_1);
  }
  if(inode_disk->indirect_block_sec > 0){
    i = 0;
    struct inode_indirect_block *ind_block = malloc(BLOCK_SECTOR_SIZE);
    bc_read(inode_disk->indirect_block_sec, ind_block, 0,
            sizeof(struct inode_indirect_block), 0);    
    while(ind_block->map_table[i] > 0){
      free_map_release(ind_block->map_table[i], 1);
      i++;
    }
    free_map_release(inode_disk->indirect_block_sec, 1);
    free(ind_block);
  }
  i = 0;
  while(inode_disk->direct_map_table[i] > 0){
    free_map_release(inode_disk->direct_map_table[i], 1);
    i++;
  }
}



bool inode_is_dir(const struct inode *inode){
  struct inode_disk *inode_disk = (struct inode_disk*)malloc(BLOCK_SECTOR_SIZE);
  if(inode->removed)
    return false;
  if(!get_disk_inode(inode, inode_disk))
    return false;
  return inode_disk->is_dir;
}