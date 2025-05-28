#include "devices/vga.h"
#include <round.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "threads/io.h"
#include "threads/interrupt.h"
#include "threads/vaddr.h"

/* VGA 텍스트 화면을 지원한다. 자세한 내용은 [FREEVGA] 참고. */

/* 텍스트 화면의 열과 행 개수. */
#define COL_CNT 80
#define ROW_CNT 25

/* 현재 커서 위치. (0,0)은 화면의 좌상단. */
static size_t cx, cy;

/* 검은 배경에 회색 글씨 속성 값. */
#define GRAY_ON_BLACK 0x07

/* 프레임버퍼. "VGA Text Mode Operation"([FREEVGA]) 참고.
   (x,y)의 문자는 fb[y][x][0], 속성은 fb[y][x][1]에 있다. */
static uint8_t (*fb)[COL_CNT][2];

static void clear_row (size_t y);
static void cls (void);
static void newline (void);
static void move_cursor (void);
static void find_cursor (size_t *x, size_t *y);

/* VGA 텍스트 화면을 초기화한다. */
static void
init (void) {
        /* 이미 초기화됐는지 확인. */
	static bool inited;
	if (!inited) {
		fb = ptov (0xb8000);
		find_cursor (&cx, &cy);
		inited = true;
	}
}

/* 제어 문자를 처리해 VGA 화면에 C를 출력한다. */
void
vga_putc (int c) {
        /* 콘솔 출력을 막기 위해 인터럽트를 잠시 비활성화한다. */
	enum intr_level old_level = intr_disable ();

	init ();

	switch (c) {
		case '\n':
			newline ();
			break;

		case '\f':
			cls ();
			break;

		case '\b':
			if (cx > 0)
				cx--;
			break;

		case '\r':
			cx = 0;
			break;

		case '\t':
			cx = ROUND_UP (cx + 1, 8);
			if (cx >= COL_CNT)
				newline ();
			break;

		default:
			fb[cy][cx][0] = c;
			fb[cy][cx][1] = GRAY_ON_BLACK;
			if (++cx >= COL_CNT)
				newline ();
			break;
	}

        /* 커서 위치를 갱신한다. */
	move_cursor ();

	intr_set_level (old_level);
}

/* 화면을 지우고 커서를 좌상단으로 이동한다. */
static void
cls (void) {
	size_t y;

	for (y = 0; y < ROW_CNT; y++)
		clear_row (y);

	cx = cy = 0;
	move_cursor ();
}

/* Y 행을 공백 문자로 채운다. */
static void
clear_row (size_t y) {
	size_t x;

	for (x = 0; x < COL_CNT; x++)
	{
		fb[y][x][0] = ' ';
		fb[y][x][1] = GRAY_ON_BLACK;
	}
}

/* 다음 줄의 첫 열로 커서를 옮긴다.
   마지막 줄에 있을 경우 화면을 한 줄 위로 올린다. */
static void
newline (void) {
	cx = 0;
	cy++;
	if (cy >= ROW_CNT)
	{
		cy = ROW_CNT - 1;
		memmove (&fb[0], &fb[1], sizeof fb[0] * (ROW_CNT - 1));
		clear_row (ROW_CNT - 1);
	}
}

/* 하드웨어 커서를 (cx,cy) 위치로 이동한다. */
static void
move_cursor (void) {
        /* "Manipulating the Text-mode Cursor"([FREEVGA]) 참고. */
	uint16_t cp = cx + COL_CNT * cy;
	outw (0x3d4, 0x0e | (cp & 0xff00));
	outw (0x3d4, 0x0f | (cp << 8));
}

/* 현재 하드웨어 커서 위치를 (*X,*Y)에 읽어 온다. */
static void
find_cursor (size_t *x, size_t *y) {
        /* "Manipulating the Text-mode Cursor"([FREEVGA]) 참고. */
	uint16_t cp;

	outb (0x3d4, 0x0e);
	cp = inb (0x3d5) << 8;

	outb (0x3d4, 0x0f);
	cp |= inb (0x3d5);

	*x = cp % COL_CNT;
	*y = cp / COL_CNT;
}
