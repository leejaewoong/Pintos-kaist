#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* 태스크 상태 세그먼트(TSS).
 *
 * x86-64에서 정의된 구조체로, 하드웨어 수준의 멀티태스킹을 위한 "태스크"를
 * 설명하지만, 이식성이나 성능상의 이유로 대부분의 운영체제는 TSS를 거의
 * 사용하지 않는다. Pintos 역시 마찬가지다.
 *
 * 다만 사용자 모드에서 발생한 인터럽트의 스택을 전환하는 일만은 TSS 없이는
 * 불가능하다. 인터럽트가 링3에서 발생하면 프로세서는 현재 TSS의 rsp0 값을
 * 참고해 사용할 스택을 결정한다. 따라서 최소한 이 필드들을 초기화한 TSS를
 * 만들어 두어야 하며, 이 파일이 그 역할을 한다.
 *
 * 인터럽트나 트랩 게이트에서 인터럽트를 처리할 때 프로세서는 다음과 같이
 * 동작한다.
 *  - 인터럽트가 같은 링에서 발생했다면 스택을 바꾸지 않는다. 즉 커널에서
 *    발생한 인터럽트라면 TSS 내용은 무시된다.
 *  - 다른 링에서 발생했다면 TSS에 지정된 스택으로 전환한다. 사용자 공간에서
 *    인터럽트가 일어날 때가 이에 해당하며, 이미 사용 중이지 않은 스택으로
 *    바꿔야 한다. 사용자 공간에서는 현재 프로세스의 커널 스택이 비어 있으니
 *    이를 사용한다. 스케줄러가 스레드를 바꿀 때도 TSS의 스택 포인터를 새
 *    스레드의 커널 스택으로 바꾼다(schedule 함수 참고). */

/* 커널용 TSS. */
struct task_state *tss;

/* 커널 TSS를 초기화한다. */
void
tss_init (void) {
	/* Our TSS is never used in a call gate or task gate, so only a
	 * few fields of it are ever referenced, and those are the only
	 * ones we initialize. */
	tss = palloc_get_page (PAL_ASSERT | PAL_ZERO);
	tss_update (thread_current ());
}

/* 커널 TSS를 반환한다. */
struct task_state *
tss_get (void) {
	ASSERT (tss != NULL);
	return tss;
}

/* TSS의 ring 0 스택 포인터를 해당 스레드 스택 끝으로 설정한다. */
void
tss_update (struct thread *next) {
	ASSERT (tss != NULL);
	tss->rsp0 = (uint64_t) next + PGSIZE;
}
