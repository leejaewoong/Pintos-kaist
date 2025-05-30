#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static int parse_cmdline(char *cmdline, char **argv, int max_arg);

/* initd를 비롯한 모든 프로세스를 위한 기본 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();	
}

/* "initd"라 불리는 첫 사용자 프로그램을 실행한다.
 * 새로 만든 스레드는 이 함수가 반환하기 전에 스케줄되어 종료될 수도 있다.
 * 성공 시 initd의 tid를 반환하고, 실패하면 TID_ERROR를 반환한다.
 * 반드시 한 번만 호출해야 한다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

        /* FILE_NAME을 그대로 사용하면 호출자와 load() 사이에 경쟁이 생길 수 있으므로 복사해 둔다.
         * 커널에서 페이지 단위로 메모리를 할당하는 palloc_get_page 함수를 호출 */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;

	strlcpy (fn_copy, file_name, PGSIZE);

	/* 파일명 분리 */
	char *save_ptr;
    strtok_r(file_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy); /* 커널 영역에 스레드 생성 실패 시 메모리 해제 */
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`이라는 새 프로세스로 복제한다.
 * 생성에 실패하면 TID_ERROR를 반환한다. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	
	struct thread *curr = thread_current();

	/* syscall handler에서 전달받은 커널 스택의 인터럽트 프레임 데이터(유저 프로그램 정보)를 스레드의 내장 프레임에 복사 */
	memcpy (&curr->backup_tf, if_, sizeof(struct intr_frame));
	
	/* 스레드를 생성한 뒤 __do_fork 실행 (부모 스레드 주소를 파라미터로 전달) */
	return thread_create (name, PRI_DEFAULT, __do_fork, curr);	
}

#ifndef VM
/* 이 함수를 pml4_for_each에 넘겨 부모의 주소 공간을 복제한다.
 * 프로젝트 2에서만 사용된다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if(is_kernel_vaddr(va))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if(parent_page == NULL)
		return true;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if(newpage == NULL)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수.
 * parent->tf에는 사용자 영역의 컨텍스트가 없으므로
 * process_fork 두 번째 인자를 이 함수에 넘겨 주어야 한다. */
static void
__do_fork (void *aux) {	
	struct thread *parent = (struct thread *) aux;
	struct thread *curr = thread_current ();	
	struct intr_frame *parent_if = &parent->backup_tf;
	struct intr_frame if_;
	bool succ = true;

        /* 1. CPU 컨텍스트를 로컬 스택으로 복사한다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

        /* 2. 페이지 테이블을 복제한다 */
	curr->pml4 = pml4_create();
	if (curr->pml4 == NULL)
		goto error;

	process_activate (curr);
#ifdef VM
	supplemental_page_table_init (&curr->spt);
	if (!supplemental_page_table_copy (&curr->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	
	/* 부모 스레드의 FDT를 순회하며 파일을 복사 후 자녀 스레드의 FDT에 저장 */	
	for(int i = 3; i < FD_MAX; i++)
	{
		if(parent->fdt[i] != NULL)		
		{
			struct file *file = file_duplicate(parent->fdt[i]);						
			curr->fdt[i] = file;
		}
	}

	/* 부모 스레드의 next_fd 값 복사 */
	curr->next_fd = parent->next_fd;

	/* 부모 스레드의 데이터 복제 후 부모 스레드 block 해제 */
	sema_up(&curr->load_sema);

	process_init ();	

        /* 마지막으로 새로 생성한 프로세스로 전환한다. */
	if (succ)
	{
		if_.R.rax = 0;
		do_iret (&if_);
	}
error:
	sema_up(&curr->load_sema);
	sys_exit(TID_ERROR);
}

/* 현재 실행 컨텍스트를 f_name으로 전환한다.
 * 실패 시 -1을 반환한다. */
int
process_exec (void *f_name) {
	/* 스레드 구조체 안에 정의된 intr_frame 필드는 인터럽트나 컨텍스트 스위치가 일어날 때, CPU의 레지스터를 저장해두는 버퍼 용도
	 * process_exec 함수에서 해당 필드를 사용하면 스레드가 다른 작업으로 바뀌었다가 돌아올 때, 스케쥴러가 같은 공간에 CPU 상태를 덮어쓰게 됨
	 * 때문에 함수 내부에 struct intr_frame _if;를 별도로 선언하여 값을 관리 */
	char *file_name = f_name;
	bool success;				
	struct intr_frame _if;

	memset(&_if, 0, sizeof(_if));	

	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

        /* 먼저 현재 실행 컨텍스트를 정리한다 */
	process_cleanup ();		
	
        /* 그다음 실행 파일을 로드한다 */
	success = load(file_name, &_if);	
	
	palloc_free_page (file_name);	
	
        /* 로드에 실패하면 종료 */
	if (!success)			
		sys_exit(-1);	

        /* 준비한 프로세스로 제어를 넘긴다 */
	do_iret (&_if);
	NOT_REACHED ();
}


int parse_cmdline(char *cmdline, char **argv, int max_arg)
{
	int argc = 0;
	char *token;
	char *saveptr;	

	token = strtok_r(cmdline, " ", &saveptr);
	while(token != NULL && argc < max_arg)
	{
		argv[argc++] = token;
		token = strtok_r(NULL, " ", &saveptr);
	}

	argv[argc] = NULL;
	return argc;
}


/* 지정한 TID의 스레드가 종료될 때까지 기다렸다가 그 종료 상태를 반환한다.
 * 커널에 의해 종료되었다면 -1을 돌려준다. TID가 잘못되었거나 호출한
 * 프로세스의 자식이 아니거나, 이미 process_wait()을 호출한 적이 있다면
 * 즉시 -1을 반환한다.
 *
 * 문제 2-2에서 구현할 함수이며, 현재는 아무 동작도 하지 않는다. */
int
process_wait (tid_t child_tid UNUSED) {
	
	int status;

	/* 자식 리스트가 비어있는 경우 함수 종료 */
	if(list_empty(&thread_current()->children))
		return -1;  			

	/* 자식 스레드 탐색 */
	struct thread *child = get_child(child_tid);
	if(child == NULL)
		return -1;
	
	/* 자식 스레드의 wait_sema 상태 확인*/		
	sema_down(&child->wait_sema);

	/* 자식 스레드 종료 상태 저장 */
	status = child->exit_status;

	/* 자식 스레드 리스트 정리 */
	list_remove(&child->child_elem);
	
	/* 자식 스레드의 종료 가능 신호 발신 */		
	sema_up(&child->exit_sema);	
	
	return status;
}

/* 프로세스를 종료한다. thread_exit()에서 호출된다. */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	/* 실행 중인 file 닫기 */
	file_close(curr->running);	

	/* fdt에 등록된 file 닫기 */
	for(int i = 3; i < FD_MAX; i++)
	{
		if(curr->fdt[i] != NULL)
		{
			file_close(curr->fdt[i]);
			curr->fdt[i] = NULL;
		}
	}

	/* FDT 정리 */
	palloc_free_page(curr->fdt);
	curr->fdt = NULL;
	
	 /* 부모에게 작업 종료 안내 */ 
	sema_up(&curr->wait_sema);
	
	/* 부모가 확인할때까지 대기 */ 
	sema_down(&curr->exit_sema);

	process_cleanup ();
}

/* 현재 프로세스가 사용 중인 자원을 해제한다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
        /* 현재 프로세스의 페이지 디렉터리를 파괴하고
         * 커널 전용 페이지 디렉터리로 되돌린다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
                /* 순서가 매우 중요하다.
                 * 타이머 인터럽트가 이전 페이지 디렉터리로 돌아가지 못하도록
                 * 우선 cur->pagedir을 NULL로 만든 뒤 페이지 디렉터리를 전환해야 한다.
                 * 또한 프로세스의 페이지 디렉터리를 파괴하기 전에
                 * 기본 페이지 디렉터리를 활성화하지 않으면 이미 해제된
                 * 디렉터리를 사용하게 된다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);		
	}
}

/* 다음 스레드가 사용자 코드를 실행할 수 있도록 CPU 상태를 준비한다.
 * 이 함수는 컨텍스트 스위치마다 호출된다. */
void
process_activate (struct thread *next) {
        /* 스레드의 페이지 테이블을 활성화한다. */
	pml4_activate (next->pml4);

        /* 인터럽트 처리에 사용할 커널 스택을 설정한다. */
	tss_update (next);
}

/* Pintos는 ELF 실행 파일을 로드한다. 아래 정의들은 [ELF1] 문서를 거의 그대로 옮긴 것이다. */

/* ELF 타입. 자세한 내용은 [ELF1] 1-2를 참고한다. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 정의 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* ELF 실행 파일 FILE_NAME을 현재 스레드에 로드한다.
 * 실행 파일의 진입점을 *RIP에,
 * 초기 스택 포인터를 *RSP에 저장한다.
 * 성공 시 true를, 실패 시 false를 반환한다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	#define MAX_ARGC 32		
	char *argv[MAX_ARGC + 1];	
	int argc;

	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;

	/* file_name을 띄어쓰기 단위로 파싱하여 argv 배열에 순차적으로 삽입 */
	argc = parse_cmdline(file_name, argv, MAX_ARGC);

	file_name = argv[0];

        /* 페이지 디렉터리를 할당하고 활성화한다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

        /* 실행 파일을 연다. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}	

        /* 실행 파일 헤더를 읽어 검증한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

        /* 프로그램 헤더를 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (int i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
                                /* 이 세그먼트는 무시한다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
                                                /* 일반 세그먼트.
                                                 * 앞부분은 디스크에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
                                                /* 완전히 0으로 채워야 하는 세그먼트.
                                                 * 디스크에서 읽어 올 내용은 없다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}
		
	/* 실행 파일 수정 금지 설정 */
	t->running = file;
	file_deny_write(file);

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;	

	char *rsp = (char *)if_->rsp;	
	char *addr[64];		
	int total = 0;

	/* 명령어의 각 인자를 마지막 원소부터 첫 원소 순으로 넣기 */
	for(int i = (argc - 1); i >= 0; i--)
	{
		int len = strlen(argv[i]) + 1;
		rsp -= len;
		strlcpy(rsp, argv[i], len);							
		addr[i] = rsp;
		total += len;
	}

	/* 패딩을 추가한 뒤에 해당 주소에 모두 0으로 초기화 */ 	
	uint8_t aligned = (total + 7) & ~0x7;
	uint8_t padding = aligned - total;	
	rsp -= padding;
	memset(rsp, 0, padding);

	/* NULL값 넣기 */	
	rsp -= sizeof(char *);
	*(void **)rsp = NULL;
	
	/* 각 명령어 인자의 주소 넣기 */	
	for(int i = (argc - 1); i >= 0; i--)
	{
		rsp -= sizeof(char *);
		*(void **)rsp = addr[i];
	}
	char **start_argv = (char **)rsp;	

	/* fake address(0) 넣기 */
	rsp -= sizeof(void *);
	*(void **)rsp = 0; 		

	/* 인터럽트 프레임의 레지스터 값 최신화 */	
	if_->R.rdi = argc;
	if_->R.rsi = (uintptr_t)start_argv;
	if_->rsp   = (uintptr_t) rsp;
	
	success = true;	

done:
	/* We arrive here whether the load is successful or not. */		
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* 파일 FILE의 OFS 위치에서 시작하는 세그먼트를 UPAGE 주소에 적재한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이 준비한다.
 *
 * - READ_BYTES 바이트는 FILE에서 읽어 온다.
 * - 나머지 ZERO_BYTES 바이트는 0으로 채운다.
 *
 * WRITABLE이 참이면 사용자 프로세스가 쓸 수 있어야 하고,
 * 그렇지 않으면 읽기 전용으로 만들어야 한다.
 *
 * 성공하면 true, 메모리 할당이나 디스크 오류가 나면 false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
                /* 이 페이지를 채우기 위해 읽을 양을 계산한다.
                 * PAGE_READ_BYTES 바이트를 파일에서 읽고
                 * 남은 PAGE_ZERO_BYTES 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

                /* 메모리 한 페이지를 할당한다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

                /* 이 페이지를 로드한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

                /* 이 페이지를 프로세스 주소 공간에 추가한다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

                /* 다음 페이지로 진행. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 초기화된 페이지를 매핑하여 최소한의 스택을 만든다 */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE는 이미 매핑되어 있지 않아야 한다.
 * KPAGE는 palloc_get_page()로 얻은 사용자 풀 페이지여야 한다.
 * 성공하면 true를, UPAGE가 이미 매핑되어 있거나
 * 메모리 할당에 실패하면 false를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

        /* 해당 가상 주소에 이미 페이지가 없는지 확인한 뒤 매핑한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 이 아래의 코드는 프로젝트 3 이후에 사용된다.
 * 프로젝트 2에서만 구현하려면 위의 블록을 수정하면 된다. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* 파일 FILE의 OFS 위치에서 시작하는 세그먼트를 UPAGE 주소에 적재한다.
 * READ_BYTES와 ZERO_BYTES의 합만큼 가상 메모리를 다음과 같이 초기화한다.
 *
 * - READ_BYTES 바이트를 FILE에서 읽어 온다.
 * - 이후 ZERO_BYTES 바이트를 0으로 채운다.
 *
 * WRITABLE이 참이면 페이지를 쓰기 가능하게 만들고,
 * 아니면 읽기 전용으로 설정한다.
 *
 * 성공하면 true, 메모리 할당 실패나 디스크 오류가 발생하면 false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
                /* 이 페이지를 채울 양을 계산한다.
                 * PAGE_READ_BYTES 바이트를 파일에서 읽고
                 * 나머지는 PAGE_ZERO_BYTES 바이트를 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

                /* TODO: lazy_load_segment에 정보를 전달할 aux를 설정한다. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

                /* 다음 주소로 이동. */
                read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK 위치에 한 페이지 크기의 스택을 생성한다. 성공 시 true를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}

#endif /* VM */
