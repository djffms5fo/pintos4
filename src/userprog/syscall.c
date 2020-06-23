#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"



static void syscall_handler (struct intr_frame *);

static void halt(void);
static tid_t exec(const char *cmd_line);
static int wait(tid_t tid);
static bool create(const char *file, unsigned initial_size);
static bool remove(const char *file);
static int open(const char *file);
static int filesize(int fd);
static int read(int fd, void *buffer, unsigned size);
static int write(int fd, void *buffer, unsigned size);
static void seek(int fd, unsigned position);
static unsigned tell(int fd);
static void close(int fd);
static bool isdir(int fd);
static bool chdir(const char *dir);
static bool mkdir(const char *dir);
static bool readdir(int fd, char *name);
static int inumber(int fd);

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
	int arg[4];
	uint32_t *sp = (uint32_t)f->esp;
	check_address((void*)sp);
	int number = *sp;
	switch (number){
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			get_argument((void*)sp, arg, 1);
			exit((int)arg[0]);
			break;
		case SYS_EXEC:
			get_argument((void*)sp, arg, 1);
			f->eax = exec((const char*)arg[0]);
			break;
		case SYS_WAIT:
			get_argument((void*)sp, arg, 1);
			f->eax = wait((tid_t)arg[0]);
			break;
		case SYS_CREATE:
			get_argument((void*)sp, arg, 2);
			f->eax = create((const char*)arg[0], (unsigned)arg[1]);
			break;
		case SYS_REMOVE:
			get_argument((void*)sp, arg, 1);
			f->eax = remove((const char*)arg[0]);
			break;
		case SYS_OPEN:
			get_argument((void*)sp, arg, 1);
			f->eax = open((const char*)arg[0]);
			break;
		case SYS_FILESIZE:
			get_argument((void*)sp, arg, 1);
			f->eax = filesize((int)arg[0]);
			break;
		case SYS_READ:
			get_argument((void*)sp, arg, 3);
			f->eax = read((int)arg[0], (void*)arg[1], (unsigned)arg[2]);
			break;
		case SYS_WRITE:
			get_argument((void*)sp, arg, 3);
			f->eax = write((int)arg[0], (void*)arg[1], (unsigned)arg[2]);
			break;
		case SYS_SEEK:
			get_argument((void*)sp, arg, 2);
			seek((int)arg[0], (unsigned)arg[1]);
			break;
		case SYS_TELL:
			get_argument((void*)sp, arg, 1);
			f->eax = tell((int)arg[0]);
			break;
		case SYS_CLOSE:
			get_argument((void*)sp, arg, 1);
			close((int)arg[0]);
			break;
		case SYS_ISDIR:
			get_argument((void*)sp, arg, 1);
			f->eax = isdir((int)arg[0]);
			break;
		case SYS_CHDIR:
			get_argument((void*)sp, arg, 1);
			f->eax = chdir((const char*)arg[0]);
			break;
		case SYS_MKDIR:
			get_argument((void*)sp, arg, 1);
			f->eax = mkdir((const char*)arg[0]);
			break;
		case SYS_READDIR:
			get_argument((void*)sp, arg, 2);
			f->eax = readdir((int)arg[0], (char*)arg[1]);
			break;
		case SYS_INUMBER:
			get_argument((void*)sp, arg, 1);
			f->eax = inumber((int)arg[0]);
			break;
		default:
			exit(-1);
	}
}

void check_address(void *addr){
	if(!is_user_vaddr(addr) && addr >= ((void*) 0x8048000)){
		exit(-1);
	}
}

void get_argument(void *esp, int *arg, int count){
	int i;
	for (i = 0; i < count; i++){
		esp += 4;
		arg[i] = *(int32_t*)esp;
		check_address((void*)arg[i]);
	}
}

void halt(void){
	shutdown_power_off();
}

void exit(int status){
	struct thread* cur = thread_current();
	cur->exit_status = status;
	printf("%s: exit(%d)\n", cur->name, status);
	thread_exit();
}

tid_t exec(const char *cmd_line){
	tid_t tid = process_execute(cmd_line);
	struct thread *child = get_child_process(tid);
	sema_down(&child->load);
	if(child->success == 0)
		return -1;
	return tid;
}

int wait(tid_t tid){
	int i = process_wait(tid);
	return i;
}

bool create(const char *file, unsigned initial_size){
	if (file == NULL)
		exit(-1);
	return filesys_create(file, initial_size);
}

bool remove(const char *file){
	if (file == NULL)
		exit(-1);
	return filesys_remove(file);
}

int open(const char *file){
	if (file == NULL)
		exit(-1);
	lock_acquire(&filesys_lock);
	struct file *f = filesys_open(file);
	if(f != NULL){
		lock_release(&filesys_lock);
		return process_add_file(f);
	}
	lock_release(&filesys_lock);
	return -1;
}

int filesize(int fd){
	struct file *f = process_get_file(fd);
	if (f != NULL)
		return file_length(f);
	return -1;
}

int read(int fd, void *buffer, unsigned size){
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(fd == 0){
		int i;
		for(i = 0; i < size; i++)
			*((char*)buffer+i) = input_getc();
		lock_release(&filesys_lock);
		return size;
	}
	if(f !=NULL){
		unsigned byte = file_read(f, buffer, size);
		lock_release(&filesys_lock);
		return byte;
	}
	lock_release(&filesys_lock);
	return -1;
}

int write(int fd, void *buffer, unsigned size){
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(fd == 1){
		putbuf(buffer, size);
		lock_release(&filesys_lock);
		return size;
	}
	if(f != NULL){
		unsigned byte = file_write(f, buffer, size);
		lock_release(&filesys_lock);
		return byte;
	}
	lock_release(&filesys_lock);
	return -1;
}

void seek(int fd, unsigned position){
	struct file *f = process_get_file(fd);
	if(f != NULL)
		file_seek(f, position);
}

unsigned tell(int fd){
	struct file *f = process_get_file(fd);
	return file_tell(f);
}

void close(int fd){
	process_close_file(fd);
}

bool isdir(int fd){
	struct file *f = process_get_file(fd);
	if(!f)
		exit(-1);
	return inode_is_dir(file_get_inode(f));
}

bool chdir(const char *dir){
	char cp_name[PATH_MAX + 1];
  	char tmp_dir[PATH_MAX + 1];
  	strlcpy(cp_name, dir, PATH_MAX);
  	strlcpy(tmp_dir, dir, PATH_MAX);
  	strlcat(tmp_dir, "/0", PATH_MAX);
  	struct dir *tmp = parse_path(tmp_dir, cp_name);

  	if(!tmp)
  		return false;


  	dir_close(thread_current()->dir);
  	thread_current()->dir = tmp;
  	return true;
}

bool mkdir(const char *dir){
	return filesys_create_dir(dir);
}

bool readdir(int fd, char *name){
	struct file *f = process_get_file(fd);
	if(!f)
		exit(-1);

	struct inode *inode = file_get_inode(f);

	if(!inode || !inode_is_dir(inode))
		return false;

	struct dir *dir = dir_open(inode);
	if(!dir)
		return false;

	bool success = true;
	int i;
	while(i <= *((off_t*)f + 1)){
		success = dir_readdir(dir, name);
		i++;
		if(!success)
			break;
	}

	if(!(i <= *((off_t*)f + 1)))
		(*((off_t*)f + 1))++;
	return success;

}

int inumber(int fd){
	struct file *f = process_get_file(fd);
	if(!f)
		exit(-1);
	return inode_get_inumber(file_get_inode(f));
}