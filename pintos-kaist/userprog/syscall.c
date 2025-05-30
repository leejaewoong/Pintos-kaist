#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

/* 파일 시스템 접근 동기화를 위한 락. */
struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜 처리.
 *
 * 예전에는 인터럽트 핸들러(예: 리눅스의 int 0x80)가 시스템 콜을 담당했지만,
 * x86-64에서는 `syscall` 명령을 통해 보다 효율적으로 요청할 수 있다.
 * 이 명령은 모델 특정 레지스터(MSR)의 값을 읽어 동작하니, 자세한 내용은 매뉴얼을 참고. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */


void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);	

        /* syscall_entry가 사용자 스택을 커널 스택으로 바꿀 때까지는
         * 인터럽트 핸들러가 다른 인터럽트를 처리하면 안 된다.
         * 그래서 FLAG_FL 비트를 마스킹해 두었다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);	

	lock_init(&filesys_lock); /* 파일입출력 lock 초기화 */	
}


void
syscall_handler (struct intr_frame *f) {		

	int code = f->R.rax;
	int num = f->R.rdi;
	int tid = thread_current()->tid;

	switch (f->R.rax)
	{
		case SYS_HALT:
			halt();		
			break;
		
		case SYS_EXIT:
			sys_exit(f->R.rdi);		
			break;

		case SYS_FORK:
			f->R.rax = fork((const char *)f->R.rdi, f);		
			break;

		case SYS_EXEC:
			f->R.rax = exec((const char *)f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = wait((tid_t)f->R.rdi);		
			break;
		
		case SYS_CREATE: 		
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE: 		
			f->R.rax = remove(f->R.rdi);
			break;
		
		case SYS_OPEN: 		
			f->R.rax = open((const char*)f->R.rdi);		
			break;

		case SYS_FILESIZE: 		
			f->R.rax = filesize(f->R.rdi);		
			break;

		case SYS_READ:
			f->R.rax = read((int)f->R.rdi, (void*)f->R.rsi, (unsigned)f->R.rdx);
			break;	

		case SYS_WRITE:
			f->R.rax = write((int)f->R.rdi, (const void*)f->R.rsi, (unsigned)f->R.rdx);
			break;

		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;

		case SYS_CLOSE:
			close(f->R.rdi);
			break;
	
	default:
		break;
	}	
}

/* 파라미터로 전달받은 주소의 유효성 검증 */
void validate_addr(const void *addr)
{
	if (addr == NULL || !is_user_vaddr (addr) || pml4_get_page (thread_current ()->pml4, addr) == NULL)
		sys_exit (-1);		
}

void halt(void)
{
	power_off();	
	return;
}

void sys_exit (int status) 
{
  struct thread *curr = thread_current ();
  
  /* 종료 상태 저장 */
  curr->exit_status = status;   
	
  thread_exit();    
}

tid_t fork(const char *thread_name, struct intr_frame *f)
{
	/* 파라미터로 전달받은 주소의 유효성 검증 */
	validate_addr(thread_name);	

	struct thread *curr = thread_current();	

	tid_t child_tid = process_fork(thread_name, f); 
	
	/* 자녀 스레드가 생성되지 않았다면 TID_ERROR 반환 */
	if (child_tid == TID_ERROR)		
		return TID_ERROR;	

	struct thread *child = get_child(child_tid);
	
	/* fork 함수 종료 전 자식 스레드가 부모 스레드의 데이터를 온전히 복제했는지 확인하기 위한 대기 */
	sema_down(&child->load_sema);	

	/* 자녀 스레드가 fork 과정에서 비정상 종료되어 종료 대기 중이라면 깨운 뒤 TID_ERROR 반환 */
	if(child->exit_status == TID_ERROR)
	{
		sema_up(&child->exit_sema);
		return TID_ERROR;
	}		
	
	return child_tid;
}

int exec(const char *cmd_line)
{
	/* 파라미터로 전달받은 주소의 유효성 검증 */
	validate_addr(cmd_line);	
	
	/* process_exec에서 초기화되는 cmd_line을 커널 영역에 복사하여 파라미터로 전달 */
	char *f_name = palloc_get_page(PAL_ZERO | PAL_ASSERT);	
	size_t size = strlen(cmd_line) + 1;	
	memcpy(f_name, cmd_line, size);

	process_exec(f_name);

	/* 메모리 해제 */
	palloc_free_page(f_name);
}

int wait(tid_t tid)
{
	int status = process_wait(tid);
	return status;
}

bool create(const char *file, unsigned initial_size)
{
	/* 파라미터 유효성 검증 */
	validate_addr(file);

	lock_acquire(&filesys_lock);

	bool result = filesys_create(file, initial_size);	

	lock_release(&filesys_lock);

	return result;
}

bool remove(const char *file) // 추가 구현 필요!
{
	/* 파라미터 유효성 검증 */
	validate_addr(file);	

	return filesys_remove(file);
}

int open(const char *file_name)
{
	/* 파라미터 유효성 검증 */
	validate_addr(file_name);	
	
	if(strcmp(file_name, "") == 0) 
		return -1;
	
	struct thread *curr = thread_current();		

	lock_acquire(&filesys_lock);

	/* 파일명과 경로 전달한 뒤 file 획득 */
	struct file *file_obj = filesys_open(file_name);
	if(file_obj == NULL) 
	{
		lock_release(&filesys_lock);
		return -1;	
	}		

	/* 할당할 fd가 없다면 에러 반환 */
	if(curr->next_fd == -1)
	{
		lock_release(&filesys_lock);
		file_close(file_obj);
		return -1;			
	}

	/* fdt에 등록 */
	int fd = curr->next_fd; 
	curr->fdt[fd] = file_obj;
	
	/* 다음에 배정할 fd 탐색하여 업데이트 */
	curr->next_fd = get_next_fd(curr);		

	lock_release(&filesys_lock);
		
	return fd;	
}

int filesize(int fd)
{
	off_t size = file_length(thread_current()->fdt[fd]);	
	return size;
}

int read(int fd, void *buffer, unsigned size)
{
	struct thread *curr = thread_current();	
	
	/* 파라미터 유효성 검증 */
	validate_addr(buffer);	

	/* 파일이 없거나 표준 입력/에러이거나 할당 가능한 fd 이상이면 종료 */
	if(fd == 0 || fd == 1 || fd > FD_MAX)
		sys_exit(-1);

	if(curr->fdt[fd] == NULL)
		sys_exit(-1);	
	
	struct file *file = curr->fdt[fd];

	/* 파라미터로 전달받은 size가 유효한지 확인 후 필요 시 size 조정 */
	if(filesize(fd) < size)
		size = filesize(fd);

	lock_acquire(&filesys_lock);
	
	/* file_read를 호출하여 실제 읽은 바이트 수를 획득 */
	off_t bytes_read = file_read(file, buffer, (off_t)size);

	lock_release(&filesys_lock);

	return (int)bytes_read;
}

int write(int fd, const void *buffer, unsigned size)
{
	/* 파라미터 유효성 검증 */
	validate_addr(buffer);	
	
	/* 표준 출력 이용 */
	if(fd == 1)
	{
		putbuf(buffer, size); 
		return size;
	}
	
	/* 실행 중인 스레드의 fd_table을 확인하여 fd에 매핑되는 file 정의 */		
	else if(fd > 2 && fd < FD_MAX)
	{		
		lock_acquire(&filesys_lock);

		struct file *file = thread_current()->fdt[fd];

		/* file_write를 호출하여 실제 쓴 바이트 수를 획득 */
		off_t bytes_written = file_write(file, buffer, size);

		lock_release(&filesys_lock);
		
		return bytes_written;
	}	

	sys_exit(-1);
}

void seek(int fd, unsigned position)
{
	file_seek(thread_current()->fdt[fd], position);	
	return; 
}

unsigned tell(int fd)
{
	off_t position = file_tell(thread_current()->fdt[fd]);	
	return position;
}

void close(int fd)
{
	struct thread *curr = thread_current();	
	
	/* 파일이 없거나 표준 입력/에러이거나 할당 가능한 fd 이상이면 종료 */
	if(fd == 0 || fd == 2 || fd > FD_MAX)
		sys_exit(-1);

	if(curr->fdt[fd] == NULL)
		sys_exit(-1);		

	struct file *file = curr->fdt[fd];
		
	file_close(file); 

	/* 할당 받은 파일 디스크립터를 NULL로 처리 */
	curr->fdt[fd] = NULL;

	return;
}

struct thread* get_child(tid_t tid)
{
	struct thread *curr = thread_current();
	
	/* 자식 리스트를 순회하며 파라미터로 받은 tid가 있는지 확인 */
	for(struct list_elem *e = list_begin(&curr->children); e != list_end(&curr->children); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, child_elem);
		if(tid == t->tid)
			return t;				
	}

	return NULL;
}

int get_next_fd(struct thread *curr)
{
	int next_fd;

	for(int i = 3; i < FD_MAX; i++) 
	{
		if(curr->fdt[i] == NULL)
		{
			next_fd = i;
			return next_fd;
		}
	}

	return -1;	
}