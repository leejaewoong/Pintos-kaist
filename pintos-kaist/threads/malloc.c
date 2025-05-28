#include "threads/malloc.h"
#include <debug.h>
#include <list.h>
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* 간단한 malloc() 구현.

   요청 크기는 2의 거듭제곱으로 올림해 "descriptor"에 배정된다.
   디스크립터는 여유 블록 목록을 관리하며, 목록이 비어 있지 않으면
   그중 하나를 사용한다.

   빈 목록이면 새로운 "arena" 페이지를 할당받아 여러 블록으로 나눈 후
   해당 디스크립터의 목록에 추가한 뒤 하나를 반환한다.

   블록을 해제할 때는 다시 목록에 넣고, 해당 arena에 남은 블록이 없다면
   arena 전체를 페이지 할당자로 돌려준다.

   이 방식으로는 2kB보다 큰 블록을 처리할 수 없으므로, 그런 경우에는
   연속된 페이지를 직접 할당하고 크기를 헤더에 기록해 둔다. */

/* 할당 블록을 관리하는 디스크립터 구조체 */
struct desc {
	size_t block_size;          /* Size of each element in bytes. */
	size_t blocks_per_arena;    /* Number of blocks in an arena. */
	struct list free_list;      /* List of free blocks. */
	struct lock lock;           /* Lock. */
};

/* arena 손상 여부를 확인하기 위한 매직 넘버 */
#define ARENA_MAGIC 0x9a548eed

/* 여러 블록을 포함하는 메모리 영역, arena */
struct arena {
	unsigned magic;             /* Always set to ARENA_MAGIC. */
	struct desc *desc;          /* Owning descriptor, null for big block. */
	size_t free_cnt;            /* Free blocks; pages in big block. */
};

/* 빈 블록을 표현 */
struct block {
	struct list_elem free_elem; /* Free list element. */
};

/* 디스크립터 테이블 */
static struct desc descs[10];   /* Descriptors. */
static size_t desc_cnt;         /* Number of descriptors. */

static struct arena *block_to_arena (struct block *);
static struct block *arena_to_block (struct arena *, size_t idx);

/* Initializes the malloc() descriptors. */
void
malloc_init (void) {
	size_t block_size;

	for (block_size = 16; block_size < PGSIZE / 2; block_size *= 2) {
		struct desc *d = &descs[desc_cnt++];
		ASSERT (desc_cnt <= sizeof descs / sizeof *descs);
		d->block_size = block_size;
		d->blocks_per_arena = (PGSIZE - sizeof (struct arena)) / block_size;
		list_init (&d->free_list);
		lock_init (&d->lock);
	}
}

/* 최소 SIZE 바이트의 새 블록을 얻어 반환한다.
   메모리가 부족하면 NULL을 반환한다. */
void *
malloc (size_t size) {
	struct desc *d;
	struct block *b;
	struct arena *a;

        /* SIZE가 0이면 NULL을 반환한다. */
	if (size == 0)
		return NULL;

        /* SIZE 바이트를 수용할 수 있는 가장 작은 디스크립터를 찾는다. */
	for (d = descs; d < descs + desc_cnt; d++)
		if (d->block_size >= size)
			break;
	if (d == descs + desc_cnt) {
                /* SIZE가 디스크립터 범위를 넘으면
                   SIZE와 arena를 담을 만큼의 페이지를 할당한다. */
		size_t page_cnt = DIV_ROUND_UP (size + sizeof *a, PGSIZE);
		a = palloc_get_multiple (0, page_cnt);
		if (a == NULL)
			return NULL;

		/* Initialize the arena to indicate a big block of PAGE_CNT
		   pages, and return it. */
		a->magic = ARENA_MAGIC;
		a->desc = NULL;
		a->free_cnt = page_cnt;
		return a + 1;
	}

	lock_acquire (&d->lock);

        /* free list가 비어 있으면 새 arena를 만든다. */
	if (list_empty (&d->free_list)) {
		size_t i;

                /* 새 페이지 할당. */
		a = palloc_get_page (0);
		if (a == NULL) {
			lock_release (&d->lock);
			return NULL;
		}

                /* arena 초기화 후 각 블록을 free list에 추가. */
		a->magic = ARENA_MAGIC;
		a->desc = d;
		a->free_cnt = d->blocks_per_arena;
		for (i = 0; i < d->blocks_per_arena; i++) {
			struct block *b = arena_to_block (a, i);
			list_push_back (&d->free_list, &b->free_elem);
		}
	}

        /* free list에서 블록 하나를 꺼내 반환. */
	b = list_entry (list_pop_front (&d->free_list), struct block, free_elem);
	a = block_to_arena (b);
	a->free_cnt--;
	lock_release (&d->lock);
	return b;
}

/* Allocates and return A times B bytes initialized to zeroes.
   Returns a null pointer if memory is not available. */
void *
calloc (size_t a, size_t b) {
	void *p;
	size_t size;

	/* Calculate block size and make sure it fits in size_t. */
	size = a * b;
	if (size < a || size < b)
		return NULL;

	/* Allocate and zero memory. */
	p = malloc (size);
	if (p != NULL)
		memset (p, 0, size);

	return p;
}

/* Returns the number of bytes allocated for BLOCK. */
static size_t
block_size (void *block) {
	struct block *b = block;
	struct arena *a = block_to_arena (b);
	struct desc *d = a->desc;

	return d != NULL ? d->block_size : PGSIZE * a->free_cnt - pg_ofs (block);
}

/* Attempts to resize OLD_BLOCK to NEW_SIZE bytes, possibly
   moving it in the process.
   If successful, returns the new block; on failure, returns a
   null pointer.
   A call with null OLD_BLOCK is equivalent to malloc(NEW_SIZE).
   A call with zero NEW_SIZE is equivalent to free(OLD_BLOCK). */
void *
realloc (void *old_block, size_t new_size) {
	if (new_size == 0) {
		free (old_block);
		return NULL;
	} else {
		void *new_block = malloc (new_size);
		if (old_block != NULL && new_block != NULL) {
			size_t old_size = block_size (old_block);
			size_t min_size = new_size < old_size ? new_size : old_size;
			memcpy (new_block, old_block, min_size);
			free (old_block);
		}
		return new_block;
	}
}

/* Frees block P, which must have been previously allocated with
   malloc(), calloc(), or realloc(). */
void
free (void *p) {
	if (p != NULL) {
		struct block *b = p;
		struct arena *a = block_to_arena (b);
		struct desc *d = a->desc;

		if (d != NULL) {
			/* It's a normal block.  We handle it here. */

#ifndef NDEBUG
			/* Clear the block to help detect use-after-free bugs. */
			memset (b, 0xcc, d->block_size);
#endif

			lock_acquire (&d->lock);

			/* Add block to free list. */
			list_push_front (&d->free_list, &b->free_elem);

			/* If the arena is now entirely unused, free it. */
			if (++a->free_cnt >= d->blocks_per_arena) {
				size_t i;

				ASSERT (a->free_cnt == d->blocks_per_arena);
				for (i = 0; i < d->blocks_per_arena; i++) {
					struct block *b = arena_to_block (a, i);
					list_remove (&b->free_elem);
				}
				palloc_free_page (a);
			}

			lock_release (&d->lock);
		} else {
			/* It's a big block.  Free its pages. */
			palloc_free_multiple (a, a->free_cnt);
			return;
		}
	}
}

/* Returns the arena that block B is inside. */
static struct arena *
block_to_arena (struct block *b) {
	struct arena *a = pg_round_down (b);

	/* Check that the arena is valid. */
	ASSERT (a != NULL);
	ASSERT (a->magic == ARENA_MAGIC);

	/* Check that the block is properly aligned for the arena. */
	ASSERT (a->desc == NULL
			|| (pg_ofs (b) - sizeof *a) % a->desc->block_size == 0);
	ASSERT (a->desc != NULL || pg_ofs (b) == sizeof *a);

	return a;
}

/* Returns the (IDX - 1)'th block within arena A. */
static struct block *
arena_to_block (struct arena *a, size_t idx) {
	ASSERT (a != NULL);
	ASSERT (a->magic == ARENA_MAGIC);
	ASSERT (idx < a->desc->blocks_per_arena);
	return (struct block *) ((uint8_t *) a
			+ sizeof *a
			+ idx * a->desc->block_size);
}
