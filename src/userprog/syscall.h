#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct lock filesys_lock;

void syscall_init (void);
void check_address(void *addr);
void get_argument(void *esp, int *arg, int count);

void exit(int status);

#endif /* userprog/syscall.h */


