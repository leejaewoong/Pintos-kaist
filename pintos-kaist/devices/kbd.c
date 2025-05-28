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

/* Shift 키의 현재 상태.
   눌려 있으면 true, 아니면 false. */
static bool left_shift, right_shift;    /* 왼쪽/오른쪽 Shift 키. */
static bool left_alt, right_alt;        /* 왼쪽/오른쪽 Alt 키. */
static bool left_ctrl, right_ctrl;      /* 왼쪽/오른쪽 Ctrl 키. */

/* Caps Lock 상태.
   켜져 있으면 true, 꺼져 있으면 false. */
static bool caps_lock;

/* 누른 키의 수. */
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
/* 연속된 스캔코드를 문자로 매핑한다. */
        uint8_t first_scancode;     /* 첫 스캔코드. */
        const char *chars;          /* chars[0]는 first_scancode에 대응하며
                                                                   chars[1]은 first_scancode+1에 대응한다. */
/* Shift 키와 관계없이 항상 같은 문자를 내는 키들.
   문자 대소문자는 다른 곳에서 처리한다. */
};

/* Shift를 누르지 않았을 때 사용하는 문자들. */
/* Shift를 누른 상태에서 사용하는 문자들. */
        /* 키보드 스캔코드. */
        /* 눌림은 false, 뗌은 true. */
        /* code에 대응하는 문자. */
        /* 프리픽스가 있으면 두 번째 바이트까지 읽어 온다. */
        /* 0x80 비트로 키 눌림/뗌을 구분한다
           (프리픽스 여부와 무관). */
                /* Caps Lock 토글. */
                /* 일반 문자 입력 처리. */
                        /* Ctrl과 Shift 처리.
                           Ctrl이 Shift보다 우선한다. */
                                /* 예: A는 0x41, Ctrl+A는 0x01 등. */
                        /* Alt는 상위 비트를 세트해 표현한다.
                           여기서의 0x80은 눌림/뗌 구분과 무관하다. */
                        /* 키보드 버퍼에 추가한다. */
                /* 키코드를 시프트 상태 변수로 변환한다. */
                /* 시프트 키 테이블. */
                /* 테이블을 순회하여 검색. */
/* 배열 K에서 SCANCODE를 찾아 해당 문자 값을 *C에 설정하고 true를 반환한다.
   찾지 못하면 false 를 반환하며 C는 사용하지 않는다. */
/* Shift 상태에서 다른 문자를 내는 키 정의 */
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
        /* Shift 키들의 현재 상태. */
	bool shift = left_shift || right_shift;
	bool alt = left_alt || right_alt;
	bool ctrl = left_ctrl || right_ctrl;

        /* 키보드 스캔코드. */
	unsigned code;

        /* 눌림이면 false, 뗌이면 true. */
	bool release;

        /* code 에 대응되는 문자. */
	uint8_t c;

        /* 프리픽스가 있으면 두 번째 바이트까지 읽어 스캔코드를 구한다. */
	code = inb (DATA_REG);
	if (code == 0xe0)
		code = (code << 8) | inb (DATA_REG);

        /* 0x80 비트가 키의 눌림과 뗌을 구분한다
           (프리픽스 여부와 무관). */
	release = (code & 0x80) != 0;
	code &= ~0x80u;

        /* 스캔코드를 실제 키 동작으로 해석한다. */
	if (code == 0x3a) {
		/* Caps Lock. */
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

                        /* Alt는 상위 비트를 설정해 표현한다.
                           여기서의 0x80은 눌림/뗌 구분에 쓰인 값과 관계없다. */
			if (alt)
				c += 0x80;

                        /* 키보드 버퍼에 추가한다. */
			if (!input_full ()) {
				key_cnt++;
				input_putc (c);
			}
		}
	} else {
                /* 키 코드를 시프트 상태 변수로 변환한다. */
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

                /* 테이블을 순회하여 검색. */
		for (key = shift_keys; key->scancode != 0; key++)
			if (key->scancode == code) {
				*key->state_var = !release;
				break;
			}
	}
}

/* 배열 K에서 SCANCODE에 해당하는 문자를 찾아 *C에 기록하고
   성공 여부를 반환한다. 찾지 못하면 false를 반환하며 C는 사용하지 않는다. */
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
