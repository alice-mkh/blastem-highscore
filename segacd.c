#include <stdlib.h>
#include <string.h>
#include "segacd.h"
#include "genesis.h"
#include "util.h"

#define SCD_MCLKS 50000000
#define SCD_PERIPH_RESET_CLKS (SCD_MCLKS / 10)
#define TIMER_TICK_CLKS 1536

enum {
	GA_SUB_CPU_CTRL,
	GA_MEM_MODE,
	GA_CDC_CTRL,
	GA_CDC_REG_DATA,
	GA_CDC_HOST_DATA,
	GA_CDC_DMA_ADDR,
	GA_STOP_WATCH,
	GA_COMM_FLAG,
	GA_COMM_CMD0,
	GA_COMM_CMD1,
	GA_COMM_CMD2,
	GA_COMM_CMD3,
	GA_COMM_CMD4,
	GA_COMM_CMD5,
	GA_COMM_CMD6,
	GA_COMM_CMD7,
	GA_COMM_STATUS0,
	GA_COMM_STATUS1,
	GA_COMM_STATUS2,
	GA_COMM_STATUS3,
	GA_COMM_STATUS4,
	GA_COMM_STATUS5,
	GA_COMM_STATUS6,
	GA_COMM_STATUS7,
	GA_TIMER,
	GA_INT_MASK,
	GA_CDD_FADER,
	GA_CDD_CTRL,
	GA_CDD_STATUS0,
	GA_CDD_STATUS1,
	GA_CDD_STATUS2,
	GA_CDD_STATUS3,
	GA_CDD_STATUS4,
	GA_CDD_CMD0,
	GA_CDD_CMD1,
	GA_CDD_CMD2,
	GA_CDD_CMD3,
	GA_CDD_CMD4,
	GA_FONT_COLOR,
	GA_FONT_BITS,
	GA_FONT_DATA0,
	GA_FONT_DATA1,
	GA_FONT_DATA2,
	GA_FONT_DATA3,

	GA_HINT_VECTOR = GA_CDC_REG_DATA
};
//GA_SUB_CPU_CTRL
#define BIT_IEN2       0x8000
#define BIT_IFL2       0x0100
#define BIT_LEDG       0x0200
#define BIT_LEDR       0x0100
#define BIT_SBRQ       0x0002
#define BIT_SRES       0x0001
#define BIT_PRES       0x0001
//GA_MEM_MODE
#define MASK_PROG_BANK 0x00C0
#define BIT_OVERWRITE  0x0010
#define BIT_UNDERWRITE 0x0008
#define MASK_PRIORITY  (BIT_OVERWRITE|BIT_UNDERWRITE)
#define BIT_MEM_MODE   0x0004
#define BIT_DMNA       0x0002
#define BIT_RET        0x0001

//GA_INT_MASK
#define BIT_MASK_IEN1  0x0002
#define BIT_MASK_IEN2  0x0004
#define BIT_MASK_IEN3  0x0008
#define BIT_MASK_IEN4  0x0010
#define BIT_MASK_IEN5  0x0020
#define BIT_MASK_IEN6  0x0040

static void *prog_ram_wp_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	//if (!(cd->gate_array[GA_MEM_MODE] & (1 << ((address >> 9) + 8)))) {
	if (address >= ((cd->gate_array[GA_MEM_MODE] & 0xFF00) << 1)) {
		cd->prog_ram[address >> 1] = value;
		m68k_invalidate_code_range(m68k, address, address + 2);
	}
	return vcontext;
}

static void *prog_ram_wp_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	if (address >= ((cd->gate_array[GA_MEM_MODE] & 0xFF00) << 1)) {
		((uint8_t *)cd->prog_ram)[address ^ 1] = value;
		m68k_invalidate_code_range(m68k, address, address + 1);
	}
	return vcontext;
}

static uint16_t word_ram_2M_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	//TODO: fixme for interleaving
	uint16_t* bank = m68k->mem_pointers[1];
	uint16_t raw = bank[address >> 2];
	if (address & 2) {
		return (raw & 0xF) | (raw << 4 & 0xF00);
	} else {
		return (raw >> 4 & 0xF00) | (raw >> 8 & 0xF);
	}
}

static uint8_t word_ram_2M_read8(uint32_t address, void *vcontext)
{
	uint16_t word = word_ram_2M_read16(address, vcontext);
	if (address & 1) {
		return word;
	}
	return word >> 8;
}

static void *word_ram_2M_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	value &= 0xF;
	uint16_t priority = cd->gate_array[GA_MEM_MODE] & MASK_PRIORITY;

	if (priority == BIT_OVERWRITE && !value) {
		return vcontext;
	}
	if (priority == BIT_UNDERWRITE) {
		if (!value) {
			return vcontext;
		}
		uint8_t old = word_ram_2M_read8(address, vcontext);
		if (old) {
			return vcontext;
		}
	}
	uint16_t* bank = m68k->mem_pointers[1];
	uint16_t raw = bank[address >> 2];
	uint16_t shift = ((address & 3) * 4);
	raw &= ~(0xF000 >> shift);
	raw |= value << (12 - shift);
	bank[address >> 2] = raw;
	return vcontext;
}


static void *word_ram_2M_write16(uint32_t address, void *vcontext, uint16_t value)
{
	word_ram_2M_write8(address, vcontext, value >> 8);
	return word_ram_2M_write8(address + 1, vcontext, value);
}

static uint16_t word_ram_1M_read16(uint32_t address, void *vcontext)
{
	return 0;
}

static uint8_t word_ram_1M_read8(uint32_t address, void *vcontext)
{
	return 0;
}

static void *word_ram_1M_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *word_ram_1M_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}


static uint16_t unmapped_prog_read16(uint32_t address, void *vcontext)
{
	return 0xFFFF;
}

static uint8_t unmapped_prog_read8(uint32_t address, void *vcontext)
{
	return 0xFF;
}

static void *unmapped_prog_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *unmapped_prog_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}

static uint16_t unmapped_word_read16(uint32_t address, void *vcontext)
{
	return 0xFFFF;
}

static uint8_t unmapped_word_read8(uint32_t address, void *vcontext)
{
	return 0xFF;
}

static void *unmapped_word_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *unmapped_word_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}

static uint16_t cell_image_read16(uint32_t address, void *vcontext)
{
	return 0xFFFF;
}

static uint8_t cell_image_read8(uint32_t address, void *vcontext)
{
	return 0xFF;
}

static void *cell_image_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *cell_image_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}

static uint8_t pcm_read8(uint32_t address, void *vcontext)
{
	return 0;
}

static uint16_t pcm_read16(uint32_t address, void *vcontext)
{
	return 0xFF00 | pcm_read8(address+1, vcontext);
}

static void *pcm_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}

static void *pcm_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return pcm_write8(address+1, vcontext, value);
}


static void timers_run(segacd_context *cd, uint32_t cycle)
{
	if (cycle <= cd->stopwatch_cycle) {
		return;
	}
	uint32_t ticks = (cycle - cd->stopwatch_cycle) / TIMER_TICK_CLKS;
	cd->stopwatch_cycle += ticks * TIMER_TICK_CLKS;
	cd->gate_array[GA_STOP_WATCH] += ticks;
	cd->gate_array[GA_STOP_WATCH] &= 0xFFF;
	if (ticks && !cd->timer_value) {
		--ticks;
		cd->timer_value = cd->gate_array[GA_TIMER];
	}
	if (ticks && cd->timer_value) {
		while (ticks >= (cd->timer_value + 1)) {
			ticks -= cd->timer_value + 1;
			cd->timer_value = cd->gate_array[GA_TIMER];
			cd->timer_pending = 1;
		}
		cd->timer_value -= ticks;
		if (!cd->timer_value) {
			cd->timer_pending = 1;
		}
	}
}

static uint32_t next_timer_int(segacd_context *cd)
{
	if (cd->timer_pending) {
		return cd->stopwatch_cycle;
	}
	if (cd->timer_value) {
		return cd->stopwatch_cycle + TIMER_TICK_CLKS * cd->timer_value;
	}
	if (cd->gate_array[GA_TIMER]) {
		return cd->stopwatch_cycle + TIMER_TICK_CLKS * (cd->gate_array[GA_TIMER] + 1);
	}
	return CYCLE_NEVER;
}

static void calculate_target_cycle(m68k_context * context)
{
	segacd_context *cd = context->system;
	context->int_cycle = CYCLE_NEVER;
	uint8_t mask = context->status & 0x7;
	if (mask < 3) {
		uint32_t next_timer;
		if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN3) {
			uint32_t next_timer_cycle = next_timer_int(cd);
			if (next_timer_cycle < context->int_cycle) {
				context->int_cycle = next_timer_cycle;
				context->int_num = 3;
			}
		}
		if (mask < 2) {
			if (cd->int2_cycle < context->int_cycle && (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN2)) {
				context->int_cycle = cd->int2_cycle;
				context->int_num = 2;
			}
		}
	}
	if (context->int_cycle > context->current_cycle && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	if (context->current_cycle >= context->sync_cycle) {
		context->should_return = 1;
		context->target_cycle = context->current_cycle;
		return;
	}
	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->current_cycle;
		return;
	}
	context->target_cycle = context->sync_cycle < context->int_cycle ? context->sync_cycle : context->int_cycle;
}

static uint16_t sub_gate_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t reg = address >> 1;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL: {
		uint16_t value = cd->gate_array[reg] & 0xFFFE;
		if (cd->periph_reset_cycle == CYCLE_NEVER || (m68k->current_cycle - cd->periph_reset_cycle) > SCD_PERIPH_RESET_CLKS) {
			value |= BIT_PRES;
		}
		return value;
	}
	case GA_MEM_MODE:
		return cd->gate_array[reg] & 0xFF1F;
	case GA_STOP_WATCH:
	case GA_TIMER:
		timers_run(cd, m68k->current_cycle);
		return cd->gate_array[reg];
	case GA_FONT_DATA0:
	case GA_FONT_DATA1:
	case GA_FONT_DATA2:
	case GA_FONT_DATA3: {
		uint16_t shift = 4 * (3 - (reg - GA_FONT_DATA0));
		uint16_t value = 0;
		uint16_t fg = cd->gate_array[GA_FONT_COLOR] >> 4;
		uint16_t bg = cd->gate_array[GA_FONT_COLOR] & 0xF;
		for (int i = 0; i < 4; i++) {
			uint16_t pixel = 0;
			if (cd->gate_array[GA_FONT_BITS] & 1 << (shift + i)) {
				pixel = fg;
			} else {
				pixel = bg;
			}
			value |= pixel << (i * 4);
		}
		return value;
	}
	default:
		return cd->gate_array[reg];
	}
}

static uint8_t sub_gate_read8(uint32_t address, void *vcontext)
{
	uint16_t val = sub_gate_read16(address, vcontext);
	return address & 1 ? val : val >> 8;
}

static void *sub_gate_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t reg = address >> 1;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL:
		cd->gate_array[reg] &= 0xF0;
		cd->gate_array[reg] |= value & (BIT_LEDG|BIT_LEDR);
		if (value & BIT_PRES) {
			cd->periph_reset_cycle = m68k->current_cycle;
		}
		break;
	case GA_MEM_MODE: {
		uint16_t changed = value ^ cd->gate_array[reg];
		genesis_context *gen = cd->genesis;
		if (changed & BIT_MEM_MODE) {
			//FIXME: ram banks are supposed to be interleaved when in 2M mode
			cd->gate_array[reg] &= ~BIT_DMNA;
			if (value & BIT_MEM_MODE) {
				//switch to 1M mode
				gen->m68k->mem_pointers[cd->memptr_start_index + 1] = (value & BIT_RET) ? cd->word_ram + 0x10000 : cd->word_ram;
				gen->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
				m68k->mem_pointers[0] = NULL;
				m68k->mem_pointers[1] = (value & BIT_RET) ? cd->word_ram : cd->word_ram + 0x10000;
			} else {
				//switch to 2M mode
				if (value & BIT_RET) {
					//Main CPU will have word ram
					gen->m68k->mem_pointers[cd->memptr_start_index + 1] = cd->word_ram;
					gen->m68k->mem_pointers[cd->memptr_start_index + 2] = cd->word_ram + 0x10000;
					m68k->mem_pointers[0] = NULL;
					m68k->mem_pointers[1] = NULL;
				} else {
					//sub cpu will have word ram
					gen->m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
					gen->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
					m68k->mem_pointers[0] = cd->word_ram;
					m68k->mem_pointers[1] = NULL;
				}
			}
			m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
			m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
		} else if (changed & BIT_RET) {
			if (value & BIT_MEM_MODE) {
				cd->gate_array[reg] &= ~BIT_DMNA;
				//swapping banks in 1M mode
				gen->m68k->mem_pointers[cd->memptr_start_index + 1] = (value & BIT_RET) ? cd->word_ram + 0x10000 : cd->word_ram;
				m68k->mem_pointers[1] = (value & BIT_RET) ? cd->word_ram : cd->word_ram + 0x10000;
				m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
			} else if (value & BIT_RET) {
				cd->gate_array[reg] &= ~BIT_DMNA;
				//giving word ram to main CPU in 2M mode
				gen->m68k->mem_pointers[cd->memptr_start_index + 1] = cd->word_ram;
				gen->m68k->mem_pointers[cd->memptr_start_index + 2] = cd->word_ram + 0x10000;
				m68k->mem_pointers[0] = NULL;
				m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
			} else {
				value |= BIT_RET;
			}
		}
		cd->gate_array[reg] &= 0xFFC2;
		cd->gate_array[reg] |= value & (BIT_RET|BIT_MEM_MODE|MASK_PRIORITY);
		break;
	}
	case GA_STOP_WATCH:
		//docs say you should only write zero to reset
		//mcd-verificator comments suggest any value will reset
		timers_run(cd, m68k->current_cycle);
		cd->gate_array[reg] = 0;
		break;
	case GA_COMM_FLAG:
		cd->gate_array[reg] &= 0xFF00;
		cd->gate_array[reg] |= value & 0xFF;
		break;
	case GA_COMM_STATUS0:
	case GA_COMM_STATUS1:
	case GA_COMM_STATUS2:
	case GA_COMM_STATUS3:
	case GA_COMM_STATUS4:
	case GA_COMM_STATUS5:
	case GA_COMM_STATUS6:
	case GA_COMM_STATUS7:
		//no effects for these other than saving the value
		cd->gate_array[reg] = value;
		break;
	case GA_TIMER:
		timers_run(cd, m68k->current_cycle);
		cd->gate_array[reg] = value & 0xFF;
		calculate_target_cycle(m68k);
		break;
	case GA_INT_MASK:
		cd->gate_array[reg] = value & (BIT_MASK_IEN6|BIT_MASK_IEN5|BIT_MASK_IEN4|BIT_MASK_IEN3|BIT_MASK_IEN2|BIT_MASK_IEN1);
		calculate_target_cycle(m68k);
		break;
	case GA_FONT_COLOR:
		cd->gate_array[reg] = value & 0xFF;
		break;
	case GA_FONT_BITS:
		cd->gate_array[reg] = value;
		break;
	default:
		printf("Unhandled gate array write %X:%X\n", address, value);
	}
	return vcontext;
}

static void *sub_gate_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t reg = (address & 0x1FF) >> 1;
	uint16_t value16;
	switch (address >> 1)
	{
	case GA_CDC_HOST_DATA:
	case GA_CDC_DMA_ADDR:
	case GA_STOP_WATCH:
	case GA_COMM_FLAG:
	case GA_TIMER:
	case GA_CDD_FADER:
	case GA_FONT_COLOR:
		//these registers treat all writes as word-wide
		value16 = value | (value << 8);
		break;
	default:
		if (address & 1) {
			value16 = cd->gate_array[reg] & 0xFF00 | value;
		} else {
			value16 = cd->gate_array[reg] & 0xFF | (value << 8);
		}
	}
	return sub_gate_write16(address, vcontext, value16);
}

static uint8_t can_main_access_prog(segacd_context *cd)
{
	//TODO: use actual busack
	return cd->busreq || !cd->reset;
}

static void scd_peripherals_run(segacd_context *cd, uint32_t cycle)
{
	timers_run(cd, cycle);
}

static m68k_context *sync_components(m68k_context * context, uint32_t address)
{
	segacd_context *cd = context->system;
	scd_peripherals_run(cd, context->current_cycle);
	switch (context->int_ack)
	{
	case 2:
		cd->int2_cycle = CYCLE_NEVER;
		break;
	case 3:
		cd->timer_pending = 0;
		break;
	}
	context->int_ack = 0;
	calculate_target_cycle(context);
	return context;
}

void scd_run(segacd_context *cd, uint32_t cycle)
{
	uint8_t m68k_run = !can_main_access_prog(cd);
	if (m68k_run) {
		cd->m68k->sync_cycle = cycle;
		if (cd->need_reset) {
			cd->need_reset = 0;
			m68k_reset(cd->m68k);
		} else {
			calculate_target_cycle(cd->m68k);
			resume_68k(cd->m68k);
		}
	} else {
		cd->m68k->current_cycle = cycle;
	}
	scd_peripherals_run(cd, cycle);
}

uint32_t gen_cycle_to_scd(uint32_t cycle, genesis_context *gen)
{
	return ((uint64_t)cycle) * ((uint64_t)SCD_MCLKS) / ((uint64_t)gen->normal_clock);
}

void scd_adjust_cycle(segacd_context *cd, uint32_t deduction)
{
	deduction = gen_cycle_to_scd(deduction, cd->genesis);
	cd->m68k->current_cycle -= deduction;
	cd->stopwatch_cycle -= deduction;
	if (deduction >= cd->int2_cycle) {
		cd->int2_cycle = 0;
	} else if (cd->int2_cycle != CYCLE_NEVER) {
		cd->int2_cycle -= deduction;
	}
	if (deduction >= cd->periph_reset_cycle) {
		cd->periph_reset_cycle = CYCLE_NEVER;
	} else if (cd->periph_reset_cycle != CYCLE_NEVER) {
		cd->periph_reset_cycle -= deduction;
	}
}

static uint16_t main_gate_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t scd_cycle = gen_cycle_to_scd(m68k->current_cycle, gen);
	scd_run(cd, scd_cycle);
	uint32_t offset = (address & 0x1FF) >> 1;
	switch (offset)
	{
	case GA_SUB_CPU_CTRL: {
		uint16_t value = 0;
		if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN2) {
			value |= BIT_IEN2;
		}
		if (cd->int2_cycle != CYCLE_NEVER) {
			value |= BIT_IFL2;
		}
		if (can_main_access_prog(cd)) {
			value |= BIT_SBRQ;
		}
		if (cd->reset) {
			value |= BIT_SRES;
		}
		return value;
	}
	case GA_MEM_MODE:
		//Main CPU can't read priority mode bits
		return cd->gate_array[offset] & 0xFFE7;
	case GA_HINT_VECTOR:
		return cd->rom_mut[0x72/2];
	case GA_CDC_DMA_ADDR:
		//TODO: open bus maybe?
		return 0xFFFF;
	default:
		if (offset < GA_TIMER) {
			return cd->gate_array[offset];
		}
		//TODO: open bus maybe?
		return 0xFFFF;
	}
}

static uint8_t main_gate_read8(uint32_t address, void *vcontext)
{
	uint16_t val = main_gate_read16(address & 0xFE, vcontext);
	return address & 1 ? val : val >> 8;
}

static void *main_gate_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t scd_cycle = gen_cycle_to_scd(m68k->current_cycle, gen);
	scd_run(cd, scd_cycle);
	uint32_t reg = (address & 0x1FF) >> 1;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL: {
		uint8_t old_access = can_main_access_prog(cd);
		cd->busreq = value & BIT_SBRQ;
		uint8_t old_reset = cd->reset;
		cd->reset = value & BIT_SRES;
		if (cd->reset && !old_reset) {
			cd->need_reset = 1;
		}
		if (value & BIT_IFL2) {
			cd->int2_cycle = scd_cycle;
		}
		/*cd->gate_array[reg] &= 0x7FFF;
		cd->gate_array[reg] |= value & 0x8000;*/
		uint8_t new_access = can_main_access_prog(cd);
		uint32_t bank = cd->gate_array[GA_MEM_MODE] >> 6 & 0x3;
		if (new_access) {
			if (!old_access) {
				m68k->mem_pointers[cd->memptr_start_index] = cd->prog_ram + bank * 0x10000;
				m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
			}
		} else if (old_access) {
			m68k->mem_pointers[cd->memptr_start_index] = NULL;
			m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
			m68k_invalidate_code_range(cd->m68k, bank * 0x20000, (bank + 1) * 0x20000);
		}
		break;
	}
	case GA_MEM_MODE: {
		uint16_t changed = cd->gate_array[reg] ^ value;
		//Main CPU can't write priority mode bits, MODE or RET
		cd->gate_array[reg] &= 0x001F;
		cd->gate_array[reg] |= value & 0xFFC0;
		if ((cd->gate_array[reg] & BIT_MEM_MODE)) {
			//1M mode
			if (!(value & BIT_DMNA)) {
				cd->gate_array[reg] |= BIT_DMNA;
			}
		} else {
			//2M mode
			if (changed & value & BIT_DMNA) {
				cd->gate_array[reg] |= BIT_DMNA;
				m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
				m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
				cd->m68k->mem_pointers[0] = cd->word_ram;
				cd->gate_array[reg] &= ~BIT_RET;

				m68k_invalidate_code_range(m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(cd->m68k, 0x080000, 0x0C0000);
			}
		}
		if (changed & MASK_PROG_BANK && can_main_access_prog(cd)) {
			uint32_t bank = cd->gate_array[GA_MEM_MODE] >> 6 & 0x3;
			m68k->mem_pointers[cd->memptr_start_index] = cd->prog_ram + bank * 0x10000;
			m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
		}
		break;
	}
	case GA_HINT_VECTOR:
		cd->rom_mut[0x72/2] = value;
		break;
	case GA_COMM_FLAG:
		//Main CPU can only write the upper byte;
		cd->gate_array[reg] &= 0xFF;
		cd->gate_array[reg] |= value & 0xFF00;
		break;
	case GA_COMM_CMD0:
	case GA_COMM_CMD1:
	case GA_COMM_CMD2:
	case GA_COMM_CMD3:
	case GA_COMM_CMD4:
	case GA_COMM_CMD5:
	case GA_COMM_CMD6:
	case GA_COMM_CMD7:
		//no effects for these other than saving the value
		cd->gate_array[reg] = value;
		break;
	default:
		printf("Unhandled gate array write %X:%X\n", address, value);
	}
	return vcontext;
}

static void *main_gate_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t reg = (address & 0x1FF) >> 1;
	uint16_t value16;
	switch (reg >> 1)
	{
	case GA_SUB_CPU_CTRL:
		if (address & 1) {
			value16 = value;
		} else {
			value16 = value << 8;
			if (cd->reset) {
				value16 |= BIT_SRES;
			}
			if (cd->busreq) {
				value16 |= BIT_SBRQ;
			}
		}
		break;
	case GA_HINT_VECTOR:
	case GA_COMM_FLAG:
		//writes to these regs are always treated as word wide
		value16 = value | (value << 8);
		break;
	default:
		if (address & 1) {
			value16 = cd->gate_array[reg] & 0xFF00 | value;
		} else {
			value16 = cd->gate_array[reg] & 0xFF | (value << 8);
		}
	}
	return main_gate_write16(address, vcontext, value16);
}

segacd_context *alloc_configure_segacd(system_media *media, uint32_t opts, uint8_t force_region, rom_info *info)
{
	static memmap_chunk sub_cpu_map[] = {
		{0x000000, 0x01FEFF, 0xFFFFFF, .flags=MMAP_READ | MMAP_CODE, .write_16 = prog_ram_wp_write16, .write_8 = prog_ram_wp_write8},
		{0x01FF00, 0x07FFFF, 0xFFFFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE},
		{0x080000, 0x0BFFFF, 0x03FFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE | MMAP_PTR_IDX | MMAP_FUNC_NULL, .ptr_index = 0,
			.read_16 = word_ram_2M_read16, .write_16 = word_ram_2M_write16, .read_8 = word_ram_2M_read8, .write_8 = word_ram_2M_write8},
		{0x0C0000, 0x0DFFFF, 0x01FFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE | MMAP_PTR_IDX | MMAP_FUNC_NULL, .ptr_index = 1,
			.read_16 = word_ram_1M_read16, .write_16 = word_ram_1M_write16, .read_8 = word_ram_1M_read8, .write_8 = word_ram_1M_write8},
		{0xFE0000, 0xFEFFFF, 0x003FFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_ONLY_ODD},
		{0xFF0000, 0xFF7FFF, 0x003FFF, .read_16 = pcm_read16, .write_16 = pcm_write16, .read_8 = pcm_read8, .write_8 = pcm_write8},
		{0xFF8000, 0xFF81FF, 0x0001FF, .read_16 = sub_gate_read16, .write_16 = sub_gate_write16, .read_8 = sub_gate_read8, .write_8 = sub_gate_write8}
	};

	segacd_context *cd = calloc(sizeof(segacd_context), 1);
	FILE *f = fopen("cdbios.bin", "rb");
	if (!f) {
		fatal_error("Failed to open CD firmware for reading");
	}
	long firmware_size = file_size(f);
	uint32_t adjusted_size = nearest_pow2(firmware_size);
	cd->rom = malloc(adjusted_size);
	if (firmware_size != fread(cd->rom, 1, firmware_size, f)) {
		fatal_error("Failed to read CD firmware");
	}
	cd->rom_mut = malloc(adjusted_size);
	byteswap_rom(adjusted_size, cd->rom);
	memcpy(cd->rom_mut, cd->rom, adjusted_size);
	cd->rom_mut[0x72/2] = 0xFFFF;

	//memset(info, 0, sizeof(*info));
	//tern_node *db = get_rom_db();
	//*info = configure_rom(db, media->buffer, media->size, media->chain ? media->chain->buffer : NULL, media->chain ? media->chain->size : 0, NULL, 0);

	cd->prog_ram = malloc(512*1024);
	cd->word_ram = malloc(256*1024);
	cd->pcm_ram = malloc(64*1024);
	//TODO: Load state from file
	cd->bram = malloc(8*1024);


	sub_cpu_map[0].buffer = sub_cpu_map[1].buffer = cd->prog_ram;
	sub_cpu_map[4].buffer = cd->bram;
	m68k_options *mopts = malloc(sizeof(m68k_options));
	init_m68k_opts(mopts, sub_cpu_map, sizeof(sub_cpu_map) / sizeof(*sub_cpu_map), 4, sync_components);
	cd->m68k = init_68k_context(mopts, NULL);
	cd->m68k->system = cd;
	cd->int2_cycle = CYCLE_NEVER;
	cd->busreq = 1;
	cd->busack = 1;
	cd->need_reset = 1;
	cd->reset = 1; //active low, so reset is not active on start
	cd->memptr_start_index = 0;
	cd->gate_array[1] = 1;
	cd->gate_array[0x1B] = 0x100;

	return cd;
}

memmap_chunk *segacd_main_cpu_map(segacd_context *cd, uint8_t cart_boot, uint32_t *num_chunks)
{
	static memmap_chunk main_cpu_map[] = {
		{0x000000, 0x01FFFF, 0x01FFFF, .flags=MMAP_READ},
		{0x020000, 0x03FFFF, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 0,
			.read_16 = unmapped_prog_read16, .write_16 = unmapped_prog_write16, .read_8 = unmapped_prog_read8, .write_8 = unmapped_prog_write8},
		{0x040000, 0x05FFFF, 0x01FFFF, .flags=MMAP_READ}, //first ROM alias
		//TODO: additional ROM/prog RAM aliases
		{0x200000, 0x21FFFF, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 1,
			.read_16 = unmapped_word_read16, .write_16 = unmapped_word_write16, .read_8 = unmapped_word_read8, .write_8 = unmapped_word_write8},
		{0x220000, 0x23FFFF, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 2,
			.read_16 = cell_image_read16, .write_16 = cell_image_write16, .read_8 = cell_image_read8, .write_8 = cell_image_write8},
		{0xA12000, 0xA12FFF, 0xFFFFFF, .read_16 = main_gate_read16, .write_16 = main_gate_write16, .read_8 = main_gate_read8, .write_8 = main_gate_write8}
	};
	for (int i = 0; i < sizeof(main_cpu_map) / sizeof(*main_cpu_map); i++)
	{
		if (main_cpu_map[i].start < 0x800000) {
			if (cart_boot) {
				main_cpu_map[i].start  |= 0x400000;
				main_cpu_map[i].end  |= 0x400000;
			} else {
				main_cpu_map[i].start  &= 0x3FFFFF;
				main_cpu_map[i].end  &= 0x3FFFFF;
			}
		}
	}
	//TODO: support BRAM cart
	main_cpu_map[0].buffer = cd->rom_mut;
	main_cpu_map[2].buffer = cd->rom;
	main_cpu_map[1].buffer = cd->prog_ram;
	main_cpu_map[3].buffer = cd->word_ram;
	main_cpu_map[4].buffer = cd->word_ram + 0x10000;
	*num_chunks = sizeof(main_cpu_map) / sizeof(*main_cpu_map);
	return main_cpu_map;
}
