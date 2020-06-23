#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/buffer_cache.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  bc_init();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
  thread_current()->dir = dir_open_root();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  bc_term();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char cp_name[PATH_MAX + 1];
  strlcpy(cp_name, name, PATH_MAX);
  struct dir *dir = parse_path(name, cp_name);
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, 0)
                  && dir_add (dir, cp_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char cp_name[PATH_MAX + 1];
  strlcpy(cp_name, name, PATH_MAX);
  struct dir *dir = parse_path(name, cp_name);
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, cp_name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char cp_name[PATH_MAX + 1];
  char tmp[PATH_MAX + 1];
  strlcpy(cp_name, name, PATH_MAX);
  struct dir *dir = parse_path(name, cp_name);
  struct dir *tmp_dir = NULL;
  struct inode *inode = NULL;
  bool success = false;

  dir_lookup(dir, cp_name, &inode);
  tmp_dir = dir_open(inode);

  if(!inode_is_dir(inode) || (tmp_dir && !dir_readdir(tmp_dir, tmp)))
    success = (dir && dir_remove(dir, cp_name));

  dir_close (dir); 

  if(tmp_dir)
    dir_close(tmp_dir);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  struct dir *dir = dir_open_root();
  dir_add(dir, ".", 1);
  dir_add(dir, "..", 1);
  dir_close(dir);
  free_map_close ();
  printf ("done.\n");
}

struct dir* parse_path(char *path_name,char *file_name){

  if(!path_name || !file_name || strlen(path_name) == 0)
    return NULL;

  struct dir *dir;

  char path[PATH_MAX + 1];
  strlcpy(path, path_name, PATH_MAX);

  if(path[0] == '/')
    dir = dir_open_root();
  else
    dir = dir_reopen(thread_current()->dir);

  if(!inode_is_dir(dir_get_inode(dir)))
    return NULL;

  char *savePtr;
  char *token = strtok_r(path, "/", &savePtr);
  char *nextToken = strtok_r(NULL, "/", &savePtr);

  if(!token){
    strlcpy(file_name, ".", PATH_MAX);
    return dir;
  }

  while(token && nextToken){
    struct inode *inode = NULL;
    if(!dir_lookup(dir, token, &inode)){
      dir_close(dir);
      return NULL;
    }

    if(!inode_is_dir(inode)){
      dir_close(dir);
      return NULL;
    }

    dir = dir_open(inode);

    token = nextToken;
    nextToken = strtok_r(NULL, "/", &savePtr);
  }
  strlcpy(file_name, token, PATH_MAX);
  return dir;
}

bool filesys_create_dir(const char *name){
  block_sector_t sector = 0;
  char cp_name[PATH_MAX + 1];
  strlcpy(cp_name, name, PATH_MAX);
  struct dir *dir = parse_path(name, cp_name);

  bool success = (dir != NULL
                  && free_map_allocate (1, &sector)
                  && dir_create (sector, 16)
                  && dir_add (dir, cp_name, sector));

  if(success){
    struct dir *tmp = dir_open(inode_open(sector));
    dir_add(tmp, ".", sector);
    dir_add(tmp, "..", inode_get_inumber(dir_get_inode(dir)));
    dir_close(tmp);
  }
  else{
    if(sector != 0)
      free_map_release(sector, 1);
  }
  dir_close(dir);
  return success;
}