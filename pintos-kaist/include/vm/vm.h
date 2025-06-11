#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "vm/file.h"
#include "threads/palloc.h"
#include "kernel/hash.h"
#include "threads/init.h"
#include "userprog/process.h"
#include "threads/mmu.h"
#include "filesys/file.h"
#include "threads/thread.h"

enum vm_type {
	/* 페이지가 아직 초기화되지 않음 */
	VM_UNINIT = 0,
	/* 파일과 관련되지 않은 페이지, 즉 익명 페이지 */
	VM_ANON = 1,
	/* 파일과 연결된 페이지 */
	VM_FILE = 2,
	/* 페이지 캐시를 담는 페이지 (프로젝트 4용) */
	VM_PAGE_CACHE = 3,

	/* 페이지 상태를 위한 비트 플래그 */

	/* 추가 정보 저장을 위한 보조 플래그입니다.
		* 값이 int 범위를 넘지 않는 한 원하는 만큼 추가할 수 있습니다. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* 이 값보다 커지면 안 됩니다. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "page" 구조체의 표현.
 * 일종의 "부모 클래스" 역할을 하며, 네 개의 "자식 클래스"가 있습니다:
 * uninit_page, file_page, anon_page, 그리고 페이지 캐시(project4).
 * 이 구조체에 미리 정의된 멤버는 삭제하거나 수정하지 마세요. */
struct page {
	const struct page_operations *operations;
	void *va;              /* 사용자 공간 기준의 주소 */
	struct frame *frame;   /* 프레임으로의 역참조 */	

	/* 구현 시 필요한 추가 필드 */
	struct hash_elem hash_elem;  /* page를 반환하기 위한 hash_elem */		

	bool writable;

	bool is_swaped;

	/* 타입별 데이터가 이 유니온에 결합됩니다.
	* 각 함수는 현재 어떤 유니온을 써야 할지 자동으로 판별합니다. */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;		
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* frame table 정의 */
extern struct list frame_table;

/* "frame" 구조체의 표현 */
struct frame {
	void *kva;
	struct page *page;
	struct list_elem frame_elem;
};

/* 페이지 동작을 위한 함수 테이블.
 * C 언어에서 "인터페이스"를 구현하는 한 방법으로,
 * 함수 포인터 테이블을 구조체 멤버에 두고
 * 필요할 때 호출하는 방식입니다. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);    
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt, void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	 vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

/* 신규 생성 함수 */
uint64_t get_hash (const struct hash_elem *e, void *aux);
bool cmp_page (const struct hash_elem *a, const struct hash_elem *b, void *aux);
static void vm_stack_growth (void *addr);
static struct frame *vm_get_frame(void);

#endif  /* VM_VM_H */
