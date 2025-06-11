/* file.c: 메모리 기반 파일 객체(mmaped object) 구현입니다. */

#include "vm/vm.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
void file_backed_destroy (struct page *page);

/* 이 구조체는 수정하지 않습니다. */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* file VM을 초기화합니다. */
void
vm_file_init (void) {
	
}

/* 파일 기반 페이지를 초기화합니다. */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 핸들러를 설정합니다. */
	page->operations = &file_ops;

	struct aux *aux = page->uninit.aux;	

	/* file-backed_page 초기화 */
	struct file_page *file_page = &page->file;

	file_page->aux = aux;
	file_page->modified = false;	
	
	return true;
}

/* 파일에서 내용을 읽어 페이지를 불러옵니다. */
bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;

	/* swap된 파일을 메모리에 로드 */
	if(file_read_at(file_page->aux->file, kva, file_page->aux->page_read_bytes, file_page->aux->ofs))
		return true;
}

/* 페이지의 내용을 파일에 기록하여 내보냅니다. */
static bool
file_backed_swap_out (struct page *page) {
	file_backed_destroy(page);
	return true;
}

/* 파일 기반 페이지를 파괴합니다. PAGE는 호출자가 해제합니다. */
void
file_backed_destroy (struct page *page) {	

	struct frame *target_frame = page->frame;
    struct thread *curr = thread_current();
    struct aux *aux = page->file.aux;

    if(target_frame != NULL)
    {
		/* 파일이 수정된 경우 write-back */
		if (pml4_is_dirty(curr->pml4, page->va)) 
        {
            file_write_at(aux->file, page->va, aux->page_read_bytes, aux->ofs);
            pml4_set_dirty(curr->pml4, page->va, false);
        }

		/* 자원 해제 */
        list_remove(&target_frame->frame_elem);
        pml4_clear_page(curr->pml4, page->va);
        palloc_free_page(target_frame->kva);
        free(target_frame);
        page->frame = NULL;
    }		
}

/* mmap을 수행합니다. */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t ofs) {
	
	void *start = addr;
	uint32_t read_bytes = length < (file_length(file) - ofs) ? length : (file_length(file) - ofs);
	
	while (read_bytes > 0) {
		/* 이 페이지를 채울 양을 계산한다.
		 * PAGE_READ_BYTES 바이트를 파일에서 읽고
		 * 나머지는 PAGE_ZERO_BYTES 바이트를 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE; /* 한 턴에 최대 1 페이지만큼만 read */
		size_t page_zero_bytes = PGSIZE - page_read_bytes; /* 0으로 채운 패딩 사이즈는 4KB - page_read_bytes */		

		/* aux 메모리 할당 후 필드 초기화 */
		struct aux *aux = malloc(sizeof(struct aux));
		if(aux == NULL)
			return NULL;

		aux->file = file_reopen(file); 
		aux->ofs = ofs;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;		

		if (!vm_alloc_page_with_initializer (VM_FILE, addr,
					writable, lazy_load_segment, aux))
			return NULL;

		/* 다음 주소로 이동. */
		read_bytes -= page_read_bytes;		
		ofs += page_read_bytes; /* 오프셋 업데이트 */ 
		addr += PGSIZE;
	}
	return start;
}

/* munmap을 수행합니다. */
void
do_munmap (void *addr) {
	
	addr = pg_round_down(addr);
	struct thread *curr = thread_current();

	struct page *page = spt_find_page(&curr->spt, addr);

	struct file *file = page->file.aux->file;
	
	if (page == NULL)
		return NULL;
	
	while (page->operations->type == VM_FILE && file == page->file.aux->file)
	{
		struct aux *aux = page->file.aux;

		if (pml4_is_dirty(curr->pml4, page->va)) 
		{
			file_write_at(aux->file, page->va, aux->page_read_bytes, aux->ofs);
			pml4_set_dirty(curr->pml4, page->va, false);
		}

		pml4_clear_page(curr->pml4, page->va);
		// spt_remove_page(&curr->spt, page);
		// hash_delete(&curr->spt.hash_table, &page->hash_elem);		

		addr += PGSIZE;
		page = spt_find_page(&curr->spt, addr);

		if(page == NULL)
			break;
	}
}
