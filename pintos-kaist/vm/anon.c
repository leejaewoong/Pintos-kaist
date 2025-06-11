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
	swap_disk = disk_get(1,1);		
	swap_table = bitmap_create_in_buf (1000, swap_disk, PGSIZE);	
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
	
	/* swap된 데이터를 메모리에 로드 */
	disk_read(swap_disk, page->anon.swap_idx, kva);
	
	/* swap_table 업데이트 */
	bitmap_flip (swap_table, page->anon.swap_idx);

	/* page의 swap_idx 초기화 */
	anon_page->swap_idx = -1;

	return true;
}

/* 페이지 내용을 swap 영역에 기록하여 내보냅니다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	/* 할당 가능한 스왑 슬롯 획득 */
	size_t swap_idx = bitmap_scan(swap_table, 0, 1, false);

	/* victim 페이지에 swap_idx 업데이트 */
	page->anon.swap_idx = swap_idx;

	/* victim 페이지를 swap disk에 저장 */
	disk_write(swap_disk, swap_idx, page->va);

	/* swap_table 업데이트 */
	bitmap_flip (swap_table, swap_idx);

	return true;
}

/* anonymous page를 파괴합니다. PAGE는 호출자가 해제합니다. */
static void
anon_destroy (struct page *page) {
	struct frame *target_frame = page->frame;
	struct thread *curr = thread_current();
	struct anon_page *anon_page = &page->anon;	

	/* frame table에서 제거 */
	enum intr_level old_level = intr_disable ();	
	list_remove(&target_frame->frame_elem);
	intr_set_level (old_level);

	/* swap된 페이지만 비트맵의 슬롯 업데이트 */
	if (anon_page->swap_idx > -1)
		bitmap_reset(swap_table, anon_page->swap_idx);	

	/* pml4에서 페이지 매핑 제거 (VA -> PA 연결 해제) */
	pml4_clear_page(curr->pml4, page->va);

	/* 자원 해제 */        
	palloc_free_page(target_frame->kva);
	free(target_frame);
	page->frame = NULL;	
}
