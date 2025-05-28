#include <stdbool.h>
#include "threads/thread.h"

#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

extern struct lock filesys_lock;

void syscall_init (void);
void validate_addr(const void *addr);
int wait(tid_t tid);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
tid_t fork(const char *thread_name, struct intr_frame *f);
void sys_exit (int status);
int open(const char *path);
int filesize(int fd);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
struct thread *get_child(tid_t tid);
int get_next_fd(struct thread *curr);

#endif /* userprog/syscall.h */
