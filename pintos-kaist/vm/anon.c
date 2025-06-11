/* anon.c: 디스크 이미지가 아닌 페이지, 즉 anonymous page를 위한 구현입니다. */

#include "vm/vm.h"
#include "devices/disk.h"

/* 아래 줄부터는 수정하지 마세요. */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 구조체는 수정하지 않습니다. */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* anonymous page 관련 데이터를 초기화합니다. */
void
vm_anon_init (void) {	
	/* 스왑 디스크 할당 */	
	swap_disk = disk_get(1,1);		

	/* 스왑 슬롯 개수 계산 후 비트맵 생성 */
	size_t swap_size = disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE);
	swap_table = bitmap_create(swap_size);	
}

/* 파일 매핑을 초기화합니다. */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러를 설정합니다. */
	page->operations = &anon_ops;	

	/* anon_page 초기화 */
	struct anon_page *anon_page = &page->anon;	
	anon_page->swap_idx = -1;	
	
	return true;	
}

/* swap 영역에서 내용을 읽어 페이지를 불러옵니다. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t idx = anon_page->swap_idx;

	if (idx == -1)
		return false;
	
	/* swap된 데이터를 메모리에 로드 */
	for (size_t i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++)
		disk_read(swap_disk, idx * (PGSIZE / DISK_SECTOR_SIZE) + i, kva + DISK_SECTOR_SIZE * i);
	
	/* swap_table 업데이트 */
	bitmap_reset (swap_table, idx);

	/* page의 swap_idx 초기화 */
	idx = -1;

	return true;
}

/* 페이지 내용을 swap 영역에 기록하여 내보냅니다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	struct frame *frame = page->frame;

	/* 할당 가능한 스왑 슬롯 획득 */
	size_t swap_idx = bitmap_scan(swap_table, 0, 1, false);

	/* victim 페이지를 swap disk에 저장 */
	for (size_t i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++)
		disk_write(swap_disk, swap_idx * (PGSIZE / DISK_SECTOR_SIZE) + i, frame->kva + DISK_SECTOR_SIZE * i);

	/* swap_table 업데이트 */
	bitmap_set (swap_table, swap_idx, true);
	
	/* victim 페이지에 swap_idx 업데이트 */
	page->anon.swap_idx = swap_idx;	

	/* 페이지 매핑 해제 및 자원 반환 */
	enum intr_level old_level = intr_disable();
	list_remove(&frame->frame_elem);
	intr_set_level(old_level);

	pml4_clear_page(thread_current()->pml4, page->va);
	palloc_free_page(frame->kva);
	free(frame);
	page->frame = NULL;

	return true;
}

/* anonymous page를 파괴합니다. PAGE는 호출자가 해제합니다. */
static void
anon_destroy (struct page *page) {
	struct frame *target = page->frame;
	struct thread *curr = thread_current();
	struct anon_page *anon_page = &page->anon;	

	/* frame table에서 제거 */
	if (page->frame != NULL) {
		enum intr_level old_level = intr_disable ();	
		list_remove(&target->frame_elem);
		intr_set_level(old_level);

		/* pml4에서 페이지 매핑 제거 (VA -> PA 연결 해제) */
		pml4_clear_page(curr->pml4, page->va);

		/* 자원 해제 */        
		palloc_free_page(target->kva);
		free(target);
		page->frame = NULL;	
	}

	/* swap된 페이지만 비트맵의 슬롯 업데이트 */
	if (anon_page->swap_idx != -1)
		bitmap_reset(swap_table, anon_page->swap_idx);	
	
}
