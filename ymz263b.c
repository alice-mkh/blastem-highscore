#include <string.h>
#include "ymz263b.h"
#include "backend.h"

enum {
	YMZ_SELT,
	YMZ_LSI_TEST,
	YMZ_TIMER0_LOW,
	YMZ_TIMER0_HIGH,
	YMZ_TIMER_BASE,
	YMZ_TIMER1,
	YMZ_TIMER2_LOW,
	YMZ_TIMER2_HIGH,
	YMZ_TIMER_CTRL,
	YMZ_PCM_PLAY_CTRL,
	YMZ_PCM_VOL,
	YMZ_PCM_DATA,
	YMZ_PCM_CTRL,
	YMZ_MIDI_CTRL,
	YMZ_MIDI_DATA
};

//YMZ_SELT
#define BIT_SELT    0x01

//YMZ_TIMER_CTRL
#define BIT_ST0     0x01
#define BIT_ST1     0x02
#define BIT_ST2     0x04
#define BIT_STBC    0x08
#define BIT_T0_MSK  0x10
#define BIT_T1_MSK  0x20
#define BIT_T2_MSK  0x40

#define TIMER_RUN_MASK (BIT_ST0|BIT_ST1|BIT_ST2)
#define TIMER_INT_MASK (BIT_T0_MSK|BIT_T1_MSK|BIT_T2_MSK)

//YMZ_PCM_PLAY_CTRL
#define BIT_ADP_RST 0x80

//YMZ_MIDI_CTRL
#define BIT_MSK_RRQ 0x01
#define BIT_MRC_RST 0x02
#define BIT_MSK_TRQ 0x04
#define BIT_MTR_RST 0x08
#define BIT_MSK_MOV 0x10
#define BIT_MSK_POV 0x20

#define STATUS_FIF1 0x01
#define STATUS_FIF2 0x02
#define STATUS_RRQ  0x04
#define STATUS_TRQ  0x08
#define STATUS_T0   0x10
#define STATUS_T1   0x20
#define STATUS_T2   0x40
#define STATUS_OV   0x80

#define MIDI_BYTE_DIVIDER 170

#define FIFO_EMPTY 255
void ymz263b_init(ymz263b *ymz, uint32_t clock_divider)
{
	memset(ymz, 0, sizeof(*ymz));
	ymz->clock_inc = clock_divider * 32;
	ymz->base_regs[YMZ_SELT] = 1;
	ymz->pcm[0].regs[0] = BIT_ADP_RST;
	ymz->pcm[1].regs[0] = BIT_ADP_RST;
	ymz->midi_regs[0] = BIT_MTR_RST | BIT_MRC_RST;
	ymz->midi_trs.read = ymz->midi_rcv.read = FIFO_EMPTY;
	ymz->status = 0;
}

static uint8_t fifo_empty(ymz_midi_fifo *fifo)
{
	return fifo->read == FIFO_EMPTY;
}

static uint8_t fifo_read(ymz_midi_fifo *fifo)
{
	uint8_t ret = fifo->fifo[fifo->read++];
	fifo->read &= 15;
	if (fifo->read == fifo->write) {
		fifo->read = FIFO_EMPTY;
	}
	return ret;
}

static uint8_t fifo_write(ymz_midi_fifo *fifo, uint8_t value)
{
	uint8_t overflow = fifo->read == fifo->write;
	if (fifo->read == FIFO_EMPTY) {
		fifo->read = fifo->write;
	}
	fifo->fifo[fifo->write++] = value;
	fifo->write &= 15;
	return overflow;
}

static uint8_t fifo_size(ymz_midi_fifo *fifo)
{
	if (fifo->read == FIFO_EMPTY) {
		return 0;
	}
	if (fifo->read == fifo->write) {
		return 16;
	}
	return (fifo->write - fifo->read) & 15;
}

void ymz263b_run(ymz263b *ymz, uint32_t target_cycle)
{
	uint8_t timer_ctrl = ymz->base_regs[YMZ_TIMER_CTRL];
	for (; ymz->cycle < target_cycle; ymz->cycle += ymz->clock_inc)
	{
		if (timer_ctrl & BIT_ST0) {
			if (ymz->timers[0]) {
				ymz->timers[0]--;
			} else {
				ymz->timers[0] = ymz->base_regs[YMZ_TIMER0_HIGH] << 8 | ymz->base_regs[YMZ_TIMER0_LOW];
				ymz->status |= STATUS_T0;
			}
		}
		if (timer_ctrl & BIT_STBC) {
			if (ymz->timers[3]) {
				ymz->timers[3]--;
			} else {
				ymz->timers[3] = ymz->base_regs[YMZ_TIMER1] << 8 & 0xF00;
				ymz->timers[3] |= ymz->base_regs[YMZ_TIMER_BASE];
				
				if (timer_ctrl & BIT_ST1) {
					if (ymz->timers[1]) {
						ymz->timers[1]--;
					} else {
						ymz->timers[1] = ymz->base_regs[YMZ_TIMER1] >> 4;
						ymz->status |= STATUS_T1;
					}
				}
				
				if (timer_ctrl & BIT_ST2) {
					if (ymz->timers[2]) {
						ymz->timers[2]--;
					} else {
						ymz->timers[2] = ymz->base_regs[YMZ_TIMER2_HIGH] << 8 | ymz->base_regs[YMZ_TIMER2_LOW];
						ymz->status |= STATUS_T2;
					}
				}
			}
		}
		if (!(ymz->midi_regs[0] & BIT_MTR_RST) && !fifo_empty(&ymz->midi_trs)) {
			if (ymz->midi_transmit) {
				--ymz->midi_transmit;
			} else {
				ymz->midi_transmit = MIDI_BYTE_DIVIDER - 1;
				//TODO: send this byte to MIDI device
				uint8_t byte = fifo_read(&ymz->midi_trs);
				printf("MIDI Transmit: %X\n", byte);
				if (fifo_empty(&ymz->midi_trs)) {
					ymz->status |= STATUS_TRQ;
				}
			}
		}
	}
}

uint32_t ymz263b_next_int(ymz263b *ymz)
{
	//TODO: Handle FIFO and MIDI receive interrupts
	uint8_t enabled_ints = (~ymz->base_regs[YMZ_TIMER_CTRL]) & TIMER_INT_MASK;
	if (!(ymz->base_regs[YMZ_MIDI_CTRL] & (BIT_MTR_RST|BIT_MSK_TRQ))) {
		enabled_ints |= STATUS_TRQ;
	}
	if (!enabled_ints) {
		return CYCLE_NEVER;
	}
	//Handle currently pending interrupts
	if (enabled_ints & ymz->status) {
		return ymz->cycle;
	}
	uint32_t ret = CYCLE_NEVER;
	if (enabled_ints & STATUS_TRQ) {
		uint8_t bytes = fifo_size(&ymz->midi_trs);
		if (bytes) {
			ret = ymz->cycle + (ymz->midi_transmit + 1) * ymz->clock_inc;
			if (bytes > 1) {
				ret += MIDI_BYTE_DIVIDER * ymz->clock_inc * (bytes - 1);
			}
		}
	}
	enabled_ints >>= 4;
	//If timers aren't already expired, interrupts can't fire unless the timers are enabled
	enabled_ints &= ymz->base_regs[YMZ_TIMER_CTRL];
	if (!(ymz->base_regs[YMZ_TIMER_CTRL] & BIT_STBC)) {
		//Timer 1 and Timer 2 depend on the base timer
		enabled_ints &= 1;
	}
	if (enabled_ints & BIT_ST0) {
		uint32_t t0 = ymz->cycle + (ymz->timers[0] + 1) * ymz->clock_inc;
		if (t0 < ret) {
			ret = t0;
		} 
	}
	if (enabled_ints & (BIT_ST1|BIT_ST2)) {
		uint32_t base = ymz->cycle + (ymz->timers[3] + 1) * ymz->clock_inc;
		if (base < ret) {
			uint32_t load = (ymz->base_regs[YMZ_TIMER1] << 8 & 0xF00) | ymz->base_regs[YMZ_TIMER_BASE];
			if (enabled_ints & BIT_ST1) {
				uint32_t t1 = ymz->timers[1] * (load + 1) * ymz->clock_inc;
				if (t1 < ret) {
					ret = t1;
				}
			}
			if (enabled_ints & BIT_ST2) {
				uint32_t t2 = ymz->timers[2] * (load + 1) * ymz->clock_inc;
				if (t2 < ret) {
					ret = t2;
				}
			}
		}
	}
	return ret;
}

void ymz263b_address_write(ymz263b *ymz, uint8_t value)
{
	ymz->address = value;
}

void ymz263b_data_write(ymz263b *ymz, uint32_t channel, uint8_t value)
{
	if (channel) {
		if (ymz->address >= YMZ_PCM_PLAY_CTRL && ymz->address < YMZ_MIDI_CTRL) {
			ymz->pcm[1].regs[ymz->address - YMZ_PCM_PLAY_CTRL] = value;
		}
	} else {
		if (ymz->address < YMZ_PCM_PLAY_CTRL) {
			ymz->base_regs[ymz->address] = value;
		} else if (ymz->address < YMZ_MIDI_CTRL) {
			ymz->pcm[0].regs[ymz->address - YMZ_PCM_PLAY_CTRL] = value;
		} else {
			ymz->midi_regs[ymz->address - YMZ_MIDI_CTRL] = value;
			if (ymz->address == YMZ_MIDI_DATA) {
				ymz->status &= ~STATUS_TRQ;
				if (fifo_empty(&ymz->midi_trs)) {
					ymz->midi_transmit = MIDI_BYTE_DIVIDER - 1;
				}
				fifo_write(&ymz->midi_trs, value);
			}
		}
	}
}

uint8_t ymz263b_data_read(ymz263b *ymz, uint32_t channel)
{
	//TODO: Supposedly only a few registers are actually readable
	if (channel) {
		if (ymz->address >= YMZ_PCM_PLAY_CTRL && ymz->address < YMZ_MIDI_CTRL) {
			return ymz->pcm[1].regs[ymz->address - YMZ_PCM_PLAY_CTRL];
		}
	} else {
		if (ymz->address < YMZ_PCM_PLAY_CTRL) {
			return ymz->base_regs[ymz->address];
		} else if (ymz->address < YMZ_MIDI_CTRL) {
			return ymz->pcm[0].regs[ymz->address - YMZ_PCM_PLAY_CTRL];
		} else {
			return ymz->midi_regs[ymz->address - YMZ_MIDI_CTRL];
		}
	}
	return 0XFF;
}

uint8_t ymz263b_status_read(ymz263b *ymz)
{
	uint8_t ret = ymz->status;
	ymz->status = 0;//&= ~(STATUS_T0|STATUS_T1|STATUS_T2);
	return ret;
}
