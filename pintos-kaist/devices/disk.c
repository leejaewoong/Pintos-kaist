#include "devices/disk.h"
#include <ctype.h>
#include <debug.h>
#include <stdbool.h>
#include <stdio.h>
#include "devices/timer.h"
#include "threads/io.h"
#include "threads/interrupt.h"
#include "threads/synch.h"

/* 이 파일의 코드는 ATA(IDE) 컨트롤러 인터페이스로
   [ATA-3] 규격을 최대한 준수한다. */

/* ATA 명령 블록 포트 주소. */
#define reg_data(CHANNEL) ((CHANNEL)->reg_base + 0)     /* 데이터 */
#define reg_error(CHANNEL) ((CHANNEL)->reg_base + 1)    /* 오류 */
#define reg_nsect(CHANNEL) ((CHANNEL)->reg_base + 2)    /* 섹터 수 */
#define reg_lbal(CHANNEL) ((CHANNEL)->reg_base + 3)     /* LBA 0:7 */
#define reg_lbam(CHANNEL) ((CHANNEL)->reg_base + 4)     /* LBA 15:8 */
#define reg_lbah(CHANNEL) ((CHANNEL)->reg_base + 5)     /* LBA 23:16 */
#define reg_device(CHANNEL) ((CHANNEL)->reg_base + 6)   /* Device/LBA 27:24 */
#define reg_status(CHANNEL) ((CHANNEL)->reg_base + 7)   /* 상태 (읽기 전용) */
#define reg_command(CHANNEL) reg_status (CHANNEL)       /* 명령 (쓰기 전용) */

/* ATA 제어 블록 포트 주소.
   최신 컨트롤러 지원에는 부족하지만 여기서는 충분하다. */
#define reg_ctl(CHANNEL) ((CHANNEL)->reg_base + 0x206)  /* Control (w/o). */
#define reg_alt_status(CHANNEL) reg_ctl (CHANNEL)       /* Alt Status (r/o). */

/* 대안 상태 레지스터 비트. */
#define STA_BSY 0x80            /* 장치 사용 중 */
#define STA_DRDY 0x40           /* 장치 준비 완료 */
#define STA_DRQ 0x08            /* 데이터 요청 */

/* 제어 레지스터 비트. */
#define CTL_SRST 0x04           /* 소프트웨어 리셋 */

/* 디바이스 레지스터 비트. */
#define DEV_MBS 0xa0            /* 반드시 설정해야 함 */
#define DEV_LBA 0x40            /* LBA 방식 사용 */
#define DEV_DEV 0x10            /* 장치 선택: 0=마스터, 1=슬레이브 */

/* 사용되는 명령어 모음.
   실제로는 더 많지만 여기서는 필요한 것만 정의한다. */
#define CMD_IDENTIFY_DEVICE 0xec        /* IDENTIFY DEVICE */
#define CMD_READ_SECTOR_RETRY 0x20      /* 재시도 포함 READ SECTOR */
#define CMD_WRITE_SECTOR_RETRY 0x30     /* 재시도 포함 WRITE SECTOR */

/* 하나의 ATA 장치. */
struct disk {
        char name[8];               /* 이름 예: "hd0:1" */
        struct channel *channel;    /* 디스크가 속한 채널 */
        int dev_no;                 /* 마스터/슬레이브 구분용 0 또는 1 */

        bool is_ata;                /* 1이면 ATA 디스크 */
        disk_sector_t capacity;     /* 섹터 단위 용량 (is_ata일 때) */

        long long read_cnt;         /* 읽은 섹터 수 */
        long long write_cnt;        /* 기록한 섹터 수 */
};

/* ATA 채널(컨트롤러).
   채널 하나당 최대 두 개의 디스크를 제어한다. */
struct channel {
        char name[8];               /* 이름 예: "hd0" */
        uint16_t reg_base;          /* 기본 I/O 포트 */
        uint8_t irq;                /* 사용 중인 인터럽트 */

        struct lock lock;           /* 컨트롤러 접근 시 획득해야 하는 락 */
        bool expecting_interrupt;   /* 인터럽트를 기다리는 중이면 true, 아니면 스푸리어스 */
        struct semaphore completion_wait;   /* 인터럽트 핸들러가 up 함 */

        struct disk devices[2];     /* 이 채널에 연결된 장치들 */
};

/* 표준 PC에서 볼 수 있는 두 개의 구형 ATA 채널만 지원한다. */
#define CHANNEL_CNT 2
static struct channel channels[CHANNEL_CNT];

static void reset_channel (struct channel *);
static bool check_device_type (struct disk *);
static void identify_ata_device (struct disk *);

static void select_sector (struct disk *, disk_sector_t);
static void issue_pio_command (struct channel *, uint8_t command);
static void input_sector (struct channel *, void *);
static void output_sector (struct channel *, const void *);

static void wait_until_idle (const struct disk *);
static bool wait_while_busy (const struct disk *);
static void select_device (const struct disk *);
static void select_device_wait (const struct disk *);

static void interrupt_handler (struct intr_frame *);

/* Initialize the disk subsystem and detect disks. */
void
disk_init (void) {
	size_t chan_no;

	for (chan_no = 0; chan_no < CHANNEL_CNT; chan_no++) {
		struct channel *c = &channels[chan_no];
		int dev_no;

		/* Initialize channel. */
		snprintf (c->name, sizeof c->name, "hd%zu", chan_no);
		switch (chan_no) {
			case 0:
				c->reg_base = 0x1f0;
				c->irq = 14 + 0x20;
				break;
			case 1:
				c->reg_base = 0x170;
				c->irq = 15 + 0x20;
				break;
			default:
				NOT_REACHED ();
		}
		lock_init (&c->lock);
		c->expecting_interrupt = false;
		sema_init (&c->completion_wait, 0);

		/* Initialize devices. */
		for (dev_no = 0; dev_no < 2; dev_no++) {
			struct disk *d = &c->devices[dev_no];
			snprintf (d->name, sizeof d->name, "%s:%d", c->name, dev_no);
			d->channel = c;
			d->dev_no = dev_no;

			d->is_ata = false;
			d->capacity = 0;

			d->read_cnt = d->write_cnt = 0;
		}

		/* Register interrupt handler. */
		intr_register_ext (c->irq, interrupt_handler, c->name);

		/* Reset hardware. */
		reset_channel (c);

		/* Distinguish ATA hard disks from other devices. */
		if (check_device_type (&c->devices[0]))
			check_device_type (&c->devices[1]);

		/* Read hard disk identity information. */
		for (dev_no = 0; dev_no < 2; dev_no++)
			if (c->devices[dev_no].is_ata)
				identify_ata_device (&c->devices[dev_no]);
	}

	/* DO NOT MODIFY BELOW LINES. */
	register_disk_inspect_intr ();
}

/* Prints disk statistics. */
void
disk_print_stats (void) {
	int chan_no;

	for (chan_no = 0; chan_no < CHANNEL_CNT; chan_no++) {
		int dev_no;

		for (dev_no = 0; dev_no < 2; dev_no++) {
			struct disk *d = disk_get (chan_no, dev_no);
			if (d != NULL && d->is_ata)
				printf ("%s: %lld reads, %lld writes\n",
						d->name, d->read_cnt, d->write_cnt);
		}
	}
}

/* Returns the disk numbered DEV_NO--either 0 or 1 for master or
   slave, respectively--within the channel numbered CHAN_NO.

   Pintos uses disks this way:
0:0 - boot loader, command line args, and operating system kernel
0:1 - file system
1:0 - scratch
1:1 - swap
*/
struct disk *
disk_get (int chan_no, int dev_no) {
	ASSERT (dev_no == 0 || dev_no == 1);

	if (chan_no < (int) CHANNEL_CNT) {
		struct disk *d = &channels[chan_no].devices[dev_no];
		if (d->is_ata)
			return d;
	}
	return NULL;
}

/* Returns the size of disk D, measured in DISK_SECTOR_SIZE-byte
   sectors. */
disk_sector_t
disk_size (struct disk *d) {
	ASSERT (d != NULL);

	return d->capacity;
}

/* 디스크 D의 섹터 SEC_NO를 DISK_SECTOR_SIZE 바이트 크기의 BUFFER로 읽어 온다.
   디스크 접근은 내부에서 동기화되므로 별도의 락이 필요 없다. */
void
disk_read (struct disk *d, disk_sector_t sec_no, void *buffer) {
	struct channel *c;

	ASSERT (d != NULL);
	ASSERT (buffer != NULL);

	c = d->channel;
	lock_acquire (&c->lock);
	select_sector (d, sec_no);
	issue_pio_command (c, CMD_READ_SECTOR_RETRY);
	sema_down (&c->completion_wait);
	if (!wait_while_busy (d))
		PANIC ("%s: disk read failed, sector=%"PRDSNu, d->name, sec_no);
	input_sector (c, buffer);
	d->read_cnt++;
	lock_release (&c->lock);
}

/* BUFFER가 DISK_SECTOR_SIZE 바이트를 담고 있어야 하며,
   이를 디스크 D의 섹터 SEC_NO에 기록한다.
   디스크가 데이터를 받았다고 알려온 뒤에 반환한다.
   디스크 접근은 내부에서 동기화되므로 별도의 락이 필요 없다. */
void
disk_write (struct disk *d, disk_sector_t sec_no, const void *buffer) {
	struct channel *c;

	ASSERT (d != NULL);
	ASSERT (buffer != NULL);

	c = d->channel;
	lock_acquire (&c->lock);
	select_sector (d, sec_no);
	issue_pio_command (c, CMD_WRITE_SECTOR_RETRY);
	if (!wait_while_busy (d))
		PANIC ("%s: disk write failed, sector=%"PRDSNu, d->name, sec_no);
	output_sector (c, buffer);
	sema_down (&c->completion_wait);
	d->write_cnt++;
	lock_release (&c->lock);
}

/* Disk detection and identification. */

static void print_ata_string (char *string, size_t size);

/* Resets an ATA channel and waits for any devices present on it
   to finish the reset. */
static void
reset_channel (struct channel *c) {
	bool present[2];
	int dev_no;

	/* The ATA reset sequence depends on which devices are present,
	   so we start by detecting device presence. */
	for (dev_no = 0; dev_no < 2; dev_no++) {
		struct disk *d = &c->devices[dev_no];

		select_device (d);

		outb (reg_nsect (c), 0x55);
		outb (reg_lbal (c), 0xaa);

		outb (reg_nsect (c), 0xaa);
		outb (reg_lbal (c), 0x55);

		outb (reg_nsect (c), 0x55);
		outb (reg_lbal (c), 0xaa);

		present[dev_no] = (inb (reg_nsect (c)) == 0x55
				&& inb (reg_lbal (c)) == 0xaa);
	}

	/* Issue soft reset sequence, which selects device 0 as a side effect.
	   Also enable interrupts. */
	outb (reg_ctl (c), 0);
	timer_usleep (10);
	outb (reg_ctl (c), CTL_SRST);
	timer_usleep (10);
	outb (reg_ctl (c), 0);

	timer_msleep (150);

	/* Wait for device 0 to clear BSY. */
	if (present[0]) {
		select_device (&c->devices[0]);
		wait_while_busy (&c->devices[0]);
	}

	/* Wait for device 1 to clear BSY. */
	if (present[1]) {
		int i;

		select_device (&c->devices[1]);
		for (i = 0; i < 3000; i++) {
			if (inb (reg_nsect (c)) == 1 && inb (reg_lbal (c)) == 1)
				break;
			timer_msleep (10);
		}
		wait_while_busy (&c->devices[1]);
	}
}

/* Checks whether device D is an ATA disk and sets D's is_ata
   member appropriately.  If D is device 0 (master), returns true
   if it's possible that a slave (device 1) exists on this
   channel.  If D is device 1 (slave), the return value is not
   meaningful. */
static bool
check_device_type (struct disk *d) {
	struct channel *c = d->channel;
	uint8_t error, lbam, lbah, status;

	select_device (d);

	error = inb (reg_error (c));
	lbam = inb (reg_lbam (c));
	lbah = inb (reg_lbah (c));
	status = inb (reg_status (c));

	if ((error != 1 && (error != 0x81 || d->dev_no == 1))
			|| (status & STA_DRDY) == 0
			|| (status & STA_BSY) != 0) {
		d->is_ata = false;
		return error != 0x81;
	} else {
		d->is_ata = (lbam == 0 && lbah == 0) || (lbam == 0x3c && lbah == 0xc3);
		return true;
	}
}

/* Sends an IDENTIFY DEVICE command to disk D and reads the
   response.  Initializes D's capacity member based on the result
   and prints a message describing the disk to the console. */
static void
identify_ata_device (struct disk *d) {
	struct channel *c = d->channel;
	uint16_t id[DISK_SECTOR_SIZE / 2];

	ASSERT (d->is_ata);

	/* Send the IDENTIFY DEVICE command, wait for an interrupt
	   indicating the device's response is ready, and read the data
	   into our buffer. */
	select_device_wait (d);
	issue_pio_command (c, CMD_IDENTIFY_DEVICE);
	sema_down (&c->completion_wait);
	if (!wait_while_busy (d)) {
		d->is_ata = false;
		return;
	}
	input_sector (c, id);

	/* Calculate capacity. */
	d->capacity = id[60] | ((uint32_t) id[61] << 16);

	/* Print identification message. */
	printf ("%s: detected %'"PRDSNu" sector (", d->name, d->capacity);
	if (d->capacity > 1024 / DISK_SECTOR_SIZE * 1024 * 1024)
		printf ("%"PRDSNu" GB",
				d->capacity / (1024 / DISK_SECTOR_SIZE * 1024 * 1024));
	else if (d->capacity > 1024 / DISK_SECTOR_SIZE * 1024)
		printf ("%"PRDSNu" MB", d->capacity / (1024 / DISK_SECTOR_SIZE * 1024));
	else if (d->capacity > 1024 / DISK_SECTOR_SIZE)
		printf ("%"PRDSNu" kB", d->capacity / (1024 / DISK_SECTOR_SIZE));
	else
		printf ("%"PRDSNu" byte", d->capacity * DISK_SECTOR_SIZE);
	printf (") disk, model \"");
	print_ata_string ((char *) &id[27], 40);
	printf ("\", serial \"");
	print_ata_string ((char *) &id[10], 20);
	printf ("\"\n");
}

/* Prints STRING, which consists of SIZE bytes in a funky format:
   each pair of bytes is in reverse order.  Does not print
   trailing whitespace and/or nulls. */
static void
print_ata_string (char *string, size_t size) {
	size_t i;

	/* Find the last non-white, non-null character. */
	for (; size > 0; size--) {
		int c = string[(size - 1) ^ 1];
		if (c != '\0' && !isspace (c))
			break;
	}

	/* Print. */
	for (i = 0; i < size; i++)
		printf ("%c", string[i ^ 1]);
}

/* Selects device D, waiting for it to become ready, and then
   writes SEC_NO to the disk's sector selection registers.  (We
   use LBA mode.) */
static void
select_sector (struct disk *d, disk_sector_t sec_no) {
	struct channel *c = d->channel;

	ASSERT (sec_no < d->capacity);
	ASSERT (sec_no < (1UL << 28));

	select_device_wait (d);
	outb (reg_nsect (c), 1);
	outb (reg_lbal (c), sec_no);
	outb (reg_lbam (c), sec_no >> 8);
	outb (reg_lbah (c), (sec_no >> 16));
	outb (reg_device (c),
			DEV_MBS | DEV_LBA | (d->dev_no == 1 ? DEV_DEV : 0) | (sec_no >> 24));
}

/* 채널 C에 COMMAND를 기록하고 완료 인터럽트를 받을 준비를 한다. */
static void
issue_pio_command (struct channel *c, uint8_t command) {
        /* 인터럽트가 켜져 있지 않으면 완료 핸들러가 세마포어를 올리지 못한다. */
	ASSERT (intr_get_level () == INTR_ON);

	c->expecting_interrupt = true;
	outb (reg_command (c), command);
}

/* PIO 모드로 채널 C의 데이터 레지스터에서 섹터를 읽어
   DISK_SECTOR_SIZE 바이트 크기의 SECTOR 버퍼에 저장한다. */
static void
input_sector (struct channel *c, void *sector) {
	insw (reg_data (c), sector, DISK_SECTOR_SIZE / 2);
}

/* PIO 모드로 채널 C의 데이터 레지스터에 SECTOR를 기록한다.
   버퍼는 DISK_SECTOR_SIZE 바이트 크기여야 한다. */
static void
output_sector (struct channel *c, const void *sector) {
	outsw (reg_data (c), sector, DISK_SECTOR_SIZE / 2);
}
/* 저수준 ATA 기본 동작. */
/* 컨트롤러가 유휴 상태(BSY와 DRQ 비트가 클리어) 되기를
   최대 10초까지 기다린다. 상태 레지스터를 읽으면
   대기 중인 인터럽트도 함께 지워진다. */

/* 디스크 D가 BSY 비트를 해제할 때까지 최대 30초 기다린 후
   DRQ 비트의 상태를 반환한다.
   ATA 규격상 초기화에 그 정도 시간이 걸릴 수 있다. */
	int i;

/* 채널을 설정해 디스크 D를 선택된 상태로 만든다. */
/* select_device()와 같지만 호출 전후로 채널이
   유휴 상태가 될 때까지 기다린다. */
/* ATA 인터럽트 핸들러. */
                                inb (reg_status (c));               /* 인터럽트 인식 */
                                sema_up (&c->completion_wait);      /* 대기 스레드 깨우기 */
/* 디스크의 읽기/쓰기 횟수를 검사하기 위한 도구.
 * int 0x43, 0x44를 통해 호출한다.
 * 입력:
 *   @RDX - 확인할 채널 번호
 *   @RCX - 확인할 장치 번호
 * 출력:
 *   @RAX - 해당 디스크의 읽기/쓰기 횟수 */
}
static bool
wait_while_busy (const struct disk *d) {
	struct channel *c = d->channel;
	int i;

	for (i = 0; i < 3000; i++) {
		if (i == 700)
			printf ("%s: busy, waiting...", d->name);
		if (!(inb (reg_alt_status (c)) & STA_BSY)) {
			if (i >= 700)
				printf ("ok\n");
			return (inb (reg_alt_status (c)) & STA_DRQ) != 0;
		}
		timer_msleep (10);
	}

	printf ("failed\n");
	return false;
}

/* Program D's channel so that D is now the selected disk. */
static void
select_device (const struct disk *d) {
	struct channel *c = d->channel;
	uint8_t dev = DEV_MBS;
	if (d->dev_no == 1)
		dev |= DEV_DEV;
	outb (reg_device (c), dev);
	inb (reg_alt_status (c));
	timer_nsleep (400);
}

/* Select disk D in its channel, as select_device(), but wait for
   the channel to become idle before and after. */
static void
select_device_wait (const struct disk *d) {
	wait_until_idle (d);
	select_device (d);
	wait_until_idle (d);
}

/* ATA interrupt handler. */
static void
interrupt_handler (struct intr_frame *f) {
	struct channel *c;

	for (c = channels; c < channels + CHANNEL_CNT; c++)
		if (f->vec_no == c->irq) {
			if (c->expecting_interrupt) {
				inb (reg_status (c));               /* Acknowledge interrupt. */
				sema_up (&c->completion_wait);      /* Wake up waiter. */
			} else
				printf ("%s: unexpected interrupt\n", c->name);
			return;
		}

	NOT_REACHED ();
}

static void
inspect_read_cnt (struct intr_frame *f) {
	struct disk * d = disk_get (f->R.rdx, f->R.rcx);
	f->R.rax = d->read_cnt;
}

static void
inspect_write_cnt (struct intr_frame *f) {
	struct disk * d = disk_get (f->R.rdx, f->R.rcx);
	f->R.rax = d->write_cnt;
}

/* Tool for testing disk r/w cnt. Calling this function via int 0x43 and int 0x44.
 * Input:
 *   @RDX - chan_no of disk to inspect
 *   @RCX - dev_no of disk to inspect
 * Output:
 *   @RAX - Read/Write count of disk. */
void
register_disk_inspect_intr (void) {
	intr_register_int (0x43, 3, INTR_OFF, inspect_read_cnt, "Inspect Disk Read Count");
	intr_register_int (0x44, 3, INTR_OFF, inspect_write_cnt, "Inspect Disk Write Count");
}