#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리한 페이지 폴트의 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램에서 발생 가능한 인터럽트의 핸들러를 등록한다.

   실제 유닉스 계열 OS에서는 [SV-386] 3-24, 3-25에 설명된 것처럼
   대부분의 인터럽트를 시그널 형태로 사용자 프로세스에게 전달하지만
   Pintos에서는 시그널을 지원하지 않으므로 단순히 프로세스를 종료한다.

   페이지 폴트는 예외 사항으로, 현재는 다른 예외처럼 처리하지만
   가상 메모리를 구현하려면 별도의 처리가 필요하다.

   각 예외에 대한 자세한 내용은 [IA32-v3a] 5.15 "Exception and Interrupt
   Reference"를 참고하라. */
void
exception_init (void) {
	/* These exceptions can be raised explicitly by a user program,
	   e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
	   we set DPL==3, meaning that user programs are allowed to
	   invoke them via these instructions. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* These exceptions have DPL==0, preventing user processes from
	   invoking them via the INT instruction.  They can still be
	   caused indirectly, e.g. #DE can be caused by dividing by
	   0.  */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* Most exceptions can be handled with interrupts turned on.
	   We need to disable interrupts for page faults because the
	   fault address is stored in CR2 and needs to be preserved. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스가 일으킨 것으로 보이는 예외를 처리한다. */
static void
kill (struct intr_frame *f) {
        /* 이 인터럽트는 사용자 프로세스가 발생시킨 것으로 보인다.
           예를 들어 매핑되지 않은 가상 주소에 접근해 페이지 폴트가
           발생했을 수 있다. 지금은 프로세스를 종료하지만, 나중에는
           커널에서 페이지 폴트를 처리해야 한다. 실제 유닉스 계열 OS는
           대부분의 예외를 시그널로 돌려주지만 Pintos는 시그널을 지원하지 않는다. */

        /* 인터럽트 프레임의 코드 세그먼트 값으로 예외가 어디서 발생했는지 알 수 있다. */
	switch (f->cs) {
		case SEL_UCSEG:
                        /* 사용자 코드 세그먼트라면 예상대로 사용자 예외이므로 프로세스를 종료한다. */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
                        /* 커널 코드 세그먼트라면 커널 버그를 의미한다.
                           커널에서 예외가 발생하면 안 되며(페이지 폴트가 생겨도 여기까지 오면 안 된다).
                           따라서 커널을 패닉시킨다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
                        /* 그 밖의 세그먼트에서 발생했다면 있을 수 없는 상황이므로 커널을 패닉시킨다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트 핸들러로, 가상 메모리를 구현하려면 이 뼈대를 채워야 한다.
   프로젝트 2의 해법에 따라서는 이 코드를 수정해야 할 수도 있다.

   진입하면 CR2 레지스터에 장애가 발생한 주소가 있고,
   fault 정보는 exception.h의 PF_* 매크로 형식으로 intr_frame의 error_code에 담겨 있다.
   아래 예제는 그 정보를 해석하는 방법을 보여 준다.
   자세한 내용은 [IA32-v3a] 5.15 "Exception and Interrupt Reference"의
   "Interrupt 14--Page Fault Exception(#PF)" 항목을 참고하라. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* True: not-present page, false: writing r/o page. */
	bool write;        /* True: access was write, false: access was read. */
	bool user;         /* True: access by user, false: access by kernel. */
	void *fault_addr;  /* Fault address. */

        /* 오류를 일으킨 가상 주소를 얻는다.
           코드나 데이터 어느 쪽일 수도 있으며,
           예외를 발생시킨 명령어의 주소(f->rip)와 반드시 같지는 않다. */

	fault_addr = (void *) rcr2();

        /* CR2 값을 읽기 전까지 잠시 꺼 두었던 인터럽트를 다시 활성화한다. */
	intr_enable ();


        /* 원인을 파악한다. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
        /* 프로젝트 3 이후 버전을 위한 처리. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

        /* 페이지 폴트 횟수를 기록한다. */
        page_fault_cnt++;

        /* userprog에서 종료 */
	sys_exit(-1);

        /* 진짜 오류라면 정보를 출력하고 종료한다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

