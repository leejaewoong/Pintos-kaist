#include "devices/kbd.h"
#include <ctype.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "devices/input.h"
#include "threads/interrupt.h"
#include "threads/io.h"

/* 키보드 데이터 레지스터 포트. */
#define DATA_REG 0x60

/* Shift 키들의 현재 상태.
   눌렸으면 true, 그렇지 않으면 false. */
static bool left_shift, right_shift;    /* Left and right Shift keys. */
static bool left_alt, right_alt;        /* Left and right Alt keys. */
static bool left_ctrl, right_ctrl;      /* Left and right Ctl keys. */

/* Caps Lock 상태.
   켜져 있으면 true, 꺼져 있으면 false. */
static bool caps_lock;

/* 지금까지 눌린 키의 수. */
static int64_t key_cnt;

static intr_handler_func keyboard_interrupt;

/* 키보드를 초기화한다. */
void
kbd_init (void) {
	intr_register_ext (0x21, keyboard_interrupt, "8042 Keyboard");
}

/* 키보드 통계를 출력한다. */
void
kbd_print_stats (void) {
	printf ("Keyboard: %lld keys pressed\n", key_cnt);
}

/* 연속된 스캔코드를 문자로 매핑하기 위한 구조체. */
struct keymap {
	uint8_t first_scancode;     /* First scancode. */
	const char *chars;          /* chars[0] has scancode first_scancode,
								   chars[1] has scancode first_scancode + 1,
								   and so on to the end of the string. */
};

/* Shift 키와 상관없이 항상 같은 문자를 내는 키들.
   알파벳 대소문자는 다른 곳에서 처리한다. */
static const struct keymap invariant_keymap[] = {
	{0x01, "\033"},
	{0x0e, "\b"},
	{0x0f, "\tQWERTYUIOP"},
	{0x1c, "\r"},
	{0x1e, "ASDFGHJKL"},
	{0x2c, "ZXCVBNM"},
	{0x37, "*"},
	{0x39, " "},
	{0, NULL},
};

/* Shift를 누르지 않았을 때 사용하는 문자. */
static const struct keymap unshifted_keymap[] = {
	{0x02, "1234567890-="},
	{0x1a, "[]"},
	{0x27, ";'`"},
	{0x2b, "\\"},
	{0x33, ",./"},
	{0, NULL},
};

/* Shift를 누른 상태에서 사용하는 문자. */
static const struct keymap shifted_keymap[] = {
	{0x02, "!@#$%^&*()_+"},
	{0x1a, "{}"},
	{0x27, ":\"~"},
	{0x2b, "|"},
	{0x33, "<>?"},
	{0, NULL},
};

static bool map_key (const struct keymap[], unsigned scancode, uint8_t *);

static void
keyboard_interrupt (struct intr_frame *args UNUSED) {
        /* Shift 키 상태. */
	bool shift = left_shift || right_shift;
	bool alt = left_alt || right_alt;
	bool ctrl = left_ctrl || right_ctrl;

        /* 키보드 스캔코드. */
	unsigned code;

        /* 키가 눌렸다면 false, 떼면 true. */
	bool release;

        /* `code`에 해당하는 문자. */
	uint8_t c;

        /* 프리픽스 코드면 두 번째 바이트까지 읽어 스캔코드를 구한다. */
	code = inb (DATA_REG);
	if (code == 0xe0)
		code = (code << 8) | inb (DATA_REG);

        /* 0x80 비트는 키의 눌림과 뗌을 구분한다
           (프리픽스 여부와 무관). */
	release = (code & 0x80) != 0;
	code &= ~0x80u;

        /* 스캔코드를 해석한다. */
	if (code == 0x3a) {
                /* Caps Lock 키. */
		if (!release)
			caps_lock = !caps_lock;
	} else if (map_key (invariant_keymap, code, &c)
			|| (!shift && map_key (unshifted_keymap, code, &c))
			|| (shift && map_key (shifted_keymap, code, &c))) {
                /* 일반 문자 입력. */
		if (!release) {
                        /* Ctrl과 Shift 처리.
                           Ctrl이 Shift보다 우선한다. */
			if (ctrl && c >= 0x40 && c < 0x60) {
                                /* 예: A는 0x41, Ctrl+A는 0x01 등. */
				c -= 0x40;
			} else if (shift == caps_lock)
				c = tolower (c);

                        /* Alt는 상위 비트를 세트한다.
                           여기서의 0x80은 눌림/뗌 판별용과 무관하다. */
			if (alt)
				c += 0x80;

                        /* 키보드 버퍼에 추가한다. */
			if (!input_full ()) {
				key_cnt++;
				input_putc (c);
			}
		}
	} else {
                /* 키코드를 시프트 상태 변수에 매핑한다. */
		struct shift_key {
			unsigned scancode;
			bool *state_var;
		};

                /* 시프트 키 테이블. */
		static const struct shift_key shift_keys[] = {
			{  0x2a, &left_shift},
			{  0x36, &right_shift},
			{  0x38, &left_alt},
			{0xe038, &right_alt},
			{  0x1d, &left_ctrl},
			{0xe01d, &right_ctrl},
			{0,      NULL},
		};

		const struct shift_key *key;

                /* 테이블을 순회하며 검색한다. */
		for (key = shift_keys; key->scancode != 0; key++)
			if (key->scancode == code) {
				*key->state_var = !release;
				break;
			}
	}
}

/* 배열 K에서 SCANCODE에 해당하는 문자를 찾는다.
   찾으면 *C에 저장하고 true를 반환한다.
   없으면 false를 반환하며 C는 사용하지 않는다. */
static bool
map_key (const struct keymap k[], unsigned scancode, uint8_t *c) {
	for (; k->first_scancode != 0; k++)
		if (scancode >= k->first_scancode
				&& scancode < k->first_scancode + strlen (k->chars)) {
			*c = k->chars[scancode - k->first_scancode];
			return true;
		}

	return false;
}
