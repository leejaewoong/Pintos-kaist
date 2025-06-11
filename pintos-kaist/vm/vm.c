/* vm.c: 가상 메모리 객체를 위한 일반적인 인터페이스입니다. */
/* test */
#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Global frame table. */
struct list frame_table;

/* 각 서브시스템의 초기화 코드를 호출하여
 * 가상 메모리 하위 시스템을 초기화합니다. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();

#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* 위의 줄은 수정하지 마세요. */
	/* TODO: 여기에 코드를 작성하세요. */
	/* frame table 초기화 */
	ASSERT (&frame_table != NULL);
	frame_table.head.prev = &frame_table.tail;
	frame_table.head.next = &frame_table.tail;
	frame_table.tail.prev = &frame_table.head;
	frame_table.tail.next = &frame_table.head;
}

/* 페이지의 유형을 얻습니다. 초기화된 이후에 어떤 타입이 될지
 * 알고 싶을 때 유용하며, 이 함수는 이미 완전히 구현되어 있습니다. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* 헬퍼 함수들 */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* 초기화 함수를 가지고 미리 생성해 두는 페이지 객체를 만듭니다.
 * 페이지를 직접 만들지 말고 이 함수나 `vm_alloc_page`를 통해 생성하세요. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux)
{
	/* 정의된 VM 타입이 아니면 에러 처리 */
	ASSERT(VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current()->spt;

	/* TODO: 페이지를 만들고 VM 타입에 맞는 initializer를 얻은 뒤
	 * TODO: uninit_new를 호출하여 "uninit" 페이지 구조체를 생성합니다.
	 * TODO: 호출 후 필요한 필드를 수정해야 합니다. */
	/* TODO: 생성한 페이지를 spt에 넣어주세요. */

	/* upage가 이미 사용 중인지 확인 */
	if (spt_find_page(spt, upage) == NULL)
	{

		bool (*page_initializer)(struct page *, enum vm_type, void *kva);
		struct page *new_page = malloc(sizeof(struct page));

		if (new_page == NULL)
			goto err;

		/* 타입별로 적절한 initializer를 선택 */
		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;

		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;

		default:
			free(new_page);
			goto err;
			break;
		}

		/* 새로 할당한 page 구조체를 uninit 상태로 초기화 */
		uninit_new(new_page, upage, init, type, aux, page_initializer);
		new_page->writable = writable;

		/* SPT 삽입, 실패 시 메모리 해제 후 false 반환 */
		if (!spt_insert_page(spt, new_page))
		{
			free(new_page);
			goto err;
		}

		return true;
	}

err:
	return false;
}

/* spt에서 VA에 해당하는 페이지를 찾아 반환합니다. 실패 시 NULL을 돌려줍니다. */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct thread *curr = thread_current();

	/* 전달 받은 va를 포함한 dummie_page 생성 */
	struct page dummie_page;
	dummie_page.va = pg_round_down(va); /* va가 포함된 페이지의 시작 주소 정의 */

	/* dummie_page의 hash_elem으로 SPT에 있는 실제 page의 hash_elem 획득 */
	struct hash_elem *hl = hash_find(&spt->hash_table, &dummie_page.hash_elem);

	/* hl 획득 실패 시 NULL 반환 */
	if (hl == NULL)
	{
		return NULL;
	}

	/* hl 획득 시 page 획득하여 반환 */
	struct page *page = NULL;
	page = hash_entry(hl, struct page, hash_elem);

	return page;
}

/* PAGE를 spt에 검증 후 삽입합니다. */
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page)
{

	struct page *find_page = spt_find_page(spt, page->va);
	if (find_page == NULL)
	{
		if (hash_insert(&spt->hash_table, &page->hash_elem) != NULL)
			return false;
	}
	return true;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page);
	return true;
}

/* 앞으로 쫓아낼 프레임을 얻습니다. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	struct thread *curr = thread_current();
	uint64_t curr_pml4 = curr->pml4;

	for (struct list_elem *i = &frame_table.head ; ; i = i->next)
	{
		victim = list_entry(i, struct frame, frame_elem);
		void *upage = victim->page->va;

		if (pml4_is_accessed (curr_pml4, upage))
			pml4_set_accessed (curr_pml4, upage, false);
		
		else
			return victim;		
	}
	
	NOT_REACHED();
	return NULL;
}

/* 한 페이지를 교체하고 그에 해당하는 프레임을 반환합니다.
 * 실패하면 NULL을 반환합니다.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *alternative UNUSED = vm_get_victim();
	/* TODO: victim을 swap 영역으로 내보내고, 그 프레임을 반환하세요. */

	enum vm_type type = alternative->page->operations->type;

	if(swap_out(alternative->page))
		return alternative;
	
	return NULL;
}

/* palloc()을 이용해 프레임을 얻습니다. 남는 프레임이 없다면 하나를
 * 해제하여 돌려줍니다. 즉 사용자 풀 메모리가 가득 차도 이 함수는
 * 프레임을 얻기 위해 기존 페이지를 해제한 뒤 유효한 주소를 반환합니다.*/
static struct frame *
vm_get_frame(void)
{
	/* frame 구조체와 실제 물리 페이지를 준비한다. */	
	struct frame *new_frame = malloc(sizeof(struct frame));
	if (new_frame == NULL)
		return NULL;

	new_frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);
	if (new_frame->kva == NULL)		
	{
		free(new_frame);
		return NULL;	
	}		

	/* frame 구조체 초기 값 설정 */
	new_frame->page = NULL;	

	/* 할당받은 frame을 frame table에 삽입 */
	enum intr_level old_level = intr_disable ();	
	list_push_front(&frame_table, &new_frame->frame_elem);
	intr_set_level (old_level);
	
	return new_frame;
}

/* 스택을 확장합니다. */
static void
vm_stack_growth(void *addr UNUSED)
{	
	/* fault_addr을 내림한 주소로 SPT에 삽입 및 프레임 매핑 */
	void *stack_addr = pg_round_down(addr);

	/* SPT에 uninit 타입으로 삽입 */
	bool success = vm_alloc_page(VM_ANON | VM_MARKER_0, stack_addr, true);

	/* SPT에 삽입 직후 프레임에 매핑하여 메모리에 로드 (Lazy Load X) */
	if (success)
		vm_claim_page(stack_addr);				
}

/* write_protected 페이지에서의 fault 처리 */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* 성공하면 true를 반환합니다 */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
						 bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{

	/* 페이지 폴트 유형
	 * 0. 잘못된 주소 접근 (미할당 주소)
	 * 1. Lazy_Loading
	 * 2. 스왑 아웃된 상태
	 * 3. 쓰기 권한 에러
	 * 4. 커널 주소 접근 */

	/* 쓰기 권한 에러, 또는 커널 주소 접근이면 함수 종료 */
	// if(write == true || user == false)
	// 	return false;

	struct thread *curr = thread_current();
	struct supplemental_page_table *spt UNUSED = &curr->spt;
	struct page *page = NULL;

	/* fault_addr이 유저 영역일 경우 전달받은 파라미터에서, 커널 영역일 경우 스레드의 stk_rsp에서 값 복사 */
	uint64_t *rsp;
	if (user)
		rsp = f->rsp;
	else
		rsp = curr->stk_rsp;	
	
	/* 스왑-아웃된 상태면 스왑-인 (추후 구현) */


	/* 페이지 폴트를 일으킨 va를 가지고 spt에서 page 탐색 */
	page = spt_find_page(spt, addr);

	/* 스택 성장을 요하는 fault_addr 처리 */
	if (page == NULL)
	{
		/* USER_STACK의 범위 내에 있으며 (USER_STACK ~ USER_STACK - 1MB) rsp - 8 보다는 높은 영역 내에 있는 fault_addr 처리 */
		if (addr < USER_STACK && addr >= (rsp - 8) && addr >= (void *)(USER_STACK - (1 << 20)))
		{
			vm_stack_growth(addr);
			return true;
		}
		else
			return false;
	}

	// if (write && !page->writable) // !!!!!!!!!!!!!!!!!!!!!!!!!!
	// 	return false;			  // 쓰기 권한이 없는 페이지에 write 접근

	return vm_do_claim_page(page);
}

/* 페이지를 해제합니다.
 * 이 함수는 수정하지 마세요. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* VA에 할당된 페이지를 SPT를 탐색하여 확보합니다. */
bool vm_claim_page(void *va UNUSED)
{
	struct supplemental_page_table *spt = &thread_current()->spt;

	/* 전달받은 va를 통해 page 확보 */
	struct page *page = spt_find_page(spt, va);

	/* page가 없을 경우 함수 종료 (추후 페이지 폴트 처리?)*/
	if (page == NULL)
		return false;

	return vm_do_claim_page(page);
}

/* 확보한 PAGE를 FRAME에 매핑하여 MMU 설정을 완료합니다. */
static bool
vm_do_claim_page(struct page *page)
{
	/* 매핑할 frame 획득 */
	struct frame *frame = vm_get_frame();

	/* 매핑할 frame이 없으면 frame 교체 */
	if (frame == NULL)
	{
		frame = vm_evict_frame();
			if(frame == NULL)
				return false;
	}
		

	/* page와 frame의 상호 참조 */
	frame->page = page;
	page->frame = frame;

	struct thread *t = thread_current();

	// /* 해당 가상 주소에 이미 페이지가 없는지 확인한 뒤 매핑한다. */
	bool result = (pml4_get_page(t->pml4, page->va) == NULL &&
				   pml4_set_page(t->pml4, page->va, frame->kva, page->writable));

	if (result == false)
		return false;

	return swap_in(page, frame->kva);
}

/* 새로운 supplemental page table을 초기화합니다 */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	/* SPT 내부의 해시 테이블 초기화 */
	hash_init(&spt->hash_table, get_hash, cmp_page, NULL);
}

/* src에서 dst로 supplemental page table을 복사합니다 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
	// 흐름 정리
	// 1. src 내부 hash_table의 모든 bucket에 있는 모든 hash_elem에 연결된 페이지들을 다 복사해야 함
	// 2. 복사해야 하니, vm_alloc_page를 통해서 새 페이지를 할당.
	// 3. 새 페이지를 할당한 후, ANON과 FILE의 경우 frame 연결(추후 swap 여부에 따라 분기 필요)
	// 4. frame 연결 후, frame이 가리키는 물리 메모리 주소(kva)에 있는 데이터 역시 memcpy를 통해 복사

	/* src의 hash_table에서 aux 복사 (초기화로 생성되는 정보 X) */
	dst->hash_table.aux = src->hash_table.aux;

	/* src의 hash_table 순회를 위한 구조체 선언 */
	struct hash_iterator src_hi;

	/* src_hi의 hash_elem을 dst의 hash_table의 첫번째 bucket, 첫번째 hash_elem으로 설정 */
	hash_first(&src_hi, &src->hash_table);

	struct aux *_aux;
	struct page *dst_page;
	
	/* dst의 hash_table을 순회하며, page 복사 */
	while (hash_next(&src_hi))
	{
		struct page *temp_page = hash_entry(hash_cur(&src_hi), struct page, hash_elem);

		enum vm_type type = VM_TYPE(temp_page->operations->type);	

		switch (type)
		{
			/* uninit인 경우 */ 			
			case VM_UNINIT:
				_aux = temp_page->uninit.aux;
				vm_alloc_page_with_initializer(temp_page->uninit.type, temp_page->va, temp_page->writable, temp_page->uninit.init, _aux);
				
				/* uninit은 메모리에 로드되지 않은 페이지라, vm_claim_page() 호출 없이 종료 */
				break;
			
			/* file인 경우, aux 전달 */
			case VM_FILE:
				_aux = temp_page->file.aux;
				vm_alloc_page_with_initializer(VM_FILE, temp_page->va, temp_page->writable, lazy_load_segment, _aux);
				
				/* spt 등록 후 frame 연결 */
				if (!vm_claim_page(temp_page->va))
					return false;
				
				/* frame 연결 후, 해당 frame이 가리키는 kva(물리 메모리 주소)에 쓰인 데이터 복사 */ 
				dst_page = spt_find_page(dst, temp_page->va);
				memcpy(dst_page->frame->kva, temp_page->frame->kva, PGSIZE);

				break;

			/* 그 외 (VM_ANON인 경우) */
			default:
				vm_alloc_page_with_initializer(VM_ANON, temp_page->va, temp_page->writable, NULL, NULL);
				
				/* spt 등록 후 frame 연결 */
				if (!vm_claim_page(temp_page->va))
					return false;
				
				/* frame 연결 후, 해당 frame이 가리키는 kva(물리 메모리 주소)에 쓰인 데이터 복사 */ 
				dst_page = spt_find_page(dst, temp_page->va);
				memcpy(dst_page->frame->kva, temp_page->frame->kva, PGSIZE);					
				
				break;
		}
	}

	/* src와 dst의 elem_cnt를 비교하여 복사 성공 여부 확인 */
	bool success = (dst->hash_table.elem_cnt == src->hash_table.elem_cnt ? true : false);

	return success;
}

void page_clear(struct hash_elem *e, void *aux)
{
	struct page *temp_page = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(temp_page);
}

/* supplemental page table이 가진 자원을 해제합니다 */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	/* TODO: 스레드가 보유한 모든 supplemental_page_table을 파괴하고
	 * TODO: 수정된 내용을 저장소에 모두 반영하세요. */

	// destroy하면 안됨. -> exec 중간에 사용할 수 있기 때문에!
	// 일단 clear만 해줌.
	hash_clear(&spt->hash_table, page_clear);
}

/* hash_elem으로 bucket_idx 획득 */
uint64_t get_hash(const struct hash_elem *e, void *aux)
{
	struct page *upage = hash_entry(e, struct page, hash_elem);
	void *va = upage->va;

	return hash_bytes(&va, sizeof(va));
}

/* 두 페이지간의 대소관계 비교(정렬에 큰 의미 없음) */
bool cmp_page(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
	if (a == NULL)
      return true;
   	if (b == NULL)
      return false;
	void *a_va = hash_entry(a, struct page, hash_elem)->va;
	void *b_va = hash_entry(b, struct page, hash_elem)->va;

	return (uint64_t *)a_va < (uint64_t *)b_va ? true : false;
}


