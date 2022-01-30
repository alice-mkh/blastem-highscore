#include "lc8951.h"
#include "backend.h"

enum {
	COMIN,
	IFSTAT,
	DBCL,
	DBCH,
	HEAD0,
	HEAD1,
	HEAD2,
	HEAD3,
	PTL,
	PTH,
	WAL,
	WAH,
	STAT0,
	STAT1,
	STAT2,
	STAT3,

	SBOUT = COMIN,
	IFCTRL = IFSTAT,
	DACL = HEAD0,
	DACH = HEAD1,
	DTTRG = HEAD2,
	DTACK = HEAD3,
	WAL_WRITE = PTL,
	WAH_WRITE = PTH,
	CTRL0 = WAL,
	CTRL1 = WAH,
	PTL_WRITE = STAT0,
	PTH_WRITE = STAT1,
	RESET = STAT3
};

//IFCTRL
#define BIT_CMDIEN 0x80
#define BIT_DTEIEN 0x40
#define BIT_DECIEN 0x20
#define BIT_CMDBK  0x10
#define BIT_DTWAI  0x08
#define BIT_STWAI  0x04
#define BIT_DOUTEN 0x02
#define BIT_SOUTEN 0x01

//IFSTAT
#define BIT_CMDI   0x80
#define BIT_DTEI   0x40
#define BIT_DECI   0x20
#define BIT_DTBSY  0x08
#define BIT_STBSY  0x04
#define BIT_DTEN   0x02
#define BIT_STEN   0x01

//CTRL0
#define BIT_DECEN 0x80
#define BIT_WRRQ  0x04

//STAT3
#define BIT_VALST 0x80

//datasheet timing info
//3 cycles for memory operation
//6 cycles min for DMA-mode host transfer

void lc8951_init(lc8951 *context, lcd8951_byte_recv_fun byte_handler, void *handler_data)
{
	//This seems to vary somewhat between Sega CD models
	//unclear if the difference is in the lc8951 or gate array
	context->regs[IFSTAT] = 0xFF;
	context->ar_mask = 0x1F;
	context->clock_step = (2 + 2) * 6; // external divider, internal divider + DMA period
	context->byte_handler = byte_handler;
	context->handler_data = handler_data;
	context->decode_end = CYCLE_NEVER;
	context->transfer_end = CYCLE_NEVER;
}

void lc8951_reg_write(lc8951 *context, uint8_t value)
{
	printf("CDC write %X: %X\n", context->ar, value);
	switch (context->ar)
	{
	case SBOUT:
		context->regs[context->ar] = value;
		if (context->ifctrl & BIT_SOUTEN) {
			context->regs[IFSTAT] &= ~BIT_STBSY;
		}
		break;
	case IFCTRL:
		context->ifctrl = value;
		if (!(value & BIT_SOUTEN)) {
			context->regs[IFSTAT] |= BIT_STBSY;
		}
		if (!(value & BIT_DOUTEN)) {
			context->regs[IFSTAT] |= BIT_DTBSY;
			context->transfer_end = CYCLE_NEVER;
		}
		break;
	case DBCL:
		context->regs[context->ar] = value;
		break;
	case DBCH:
		context->regs[context->ar] = value & 0xF;
		break;
	case DACL:
		context->dac &= 0xFF00;
		context->dac |= value;
		break;
	case DACH:
		context->dac &= 0xFF;
		context->dac |= value << 8;
		break;
	case DTTRG:
		if (context->ifctrl & BIT_DOUTEN) {
			context->regs[IFSTAT] &= ~BIT_DTBSY;
			uint16_t transfer_size = context->regs[DBCL] | (context->regs[DBCH] << 8);
			context->transfer_end = context->cycle + transfer_size * context->clock_step;
			printf("DTTRG: size %u, cycle %u, end %u\n", transfer_size, context->cycle, context->transfer_end);
		}
		break;
	case DTACK:
		context->regs[IFSTAT] |= BIT_DTEI;
		break;
	case WAL_WRITE:
		context->regs[WAL] = value;
		break;
	case WAH_WRITE:
		context->regs[WAH] = value;
		break;
	case CTRL0:
		context->ctrl0 = value;
		break;
	case CTRL1:
		context->ctrl1 = value;
		break;
	case PTL_WRITE:
		context->regs[PTL] = value;
		break;
	case PTH_WRITE:
		context->regs[PTH] = value;
		context->ptl_internal = (context->regs[PTL] | (context->regs[PTH] << 8)) & (sizeof(context->buffer) - 1);
		context->decoding = 1;
		context->decode_end = context->cycle + 2352 * context->clock_step * 4;
		break;
	case RESET:
		context->comin_count = 0;
		context->regs[IFSTAT] = 0xFF;
		break;
	default:
		break;
	}
	if (context->ar != SBOUT) {
		context->ar++;
		context->ar &= context->ar_mask;
	}
}

uint8_t lc8951_reg_read(lc8951 *context)
{
	uint8_t value;
	if (context->ar == COMIN) {
		if (!context->comin_count) {
			return 0xFF;
		}
		value = context->comin[(context->comin_write - context->comin_count)&sizeof(context->comin)];
		context->comin_count--;
		if (!context->comin_count) {
			context->regs[IFSTAT] |= BIT_CMDI;
		}
		return value;
	}
	if (context->ar == STAT3) {
		context->regs[IFSTAT] |= BIT_DECI;
	}
	if (context->ar >= sizeof(context->regs)) {
		value = 0xFF;
	} else {
		value = context->regs[context->ar];
	}
	printf("CDC read %X: %X\n", context->ar, value);
	context->ar++;
	context->ar &= context->ar_mask;
	return value;
}

void lc8951_ar_write(lc8951 *context, uint8_t value)
{
	context->ar = value & context->ar_mask;
}

//25 MHz clock input (1/2 SCD MCLK)
//internal /2 divider
//3 cycles for each SRAM access (though might be crystal frequency rather than internal frequency)
//6 cycle period for DMA transfer out
//

void lc8951_run(lc8951 *context, uint32_t cycle)
{
	for(; context->cycle < cycle; context->cycle += context->clock_step)
	{
		if (context->cycle >= context->decode_end) {
			context->decode_end = CYCLE_NEVER;
			context->regs[IFSTAT] &= ~BIT_DECI;
			context->regs[STAT3] &= ~BIT_VALST;
			uint16_t block_start = (context->regs[PTL] | (context->regs[PTH] << 8)) & (sizeof(context->buffer)-1);
			for (int reg = HEAD0; reg < PTL; reg++)
			{
				printf("Setting HEAD%d to buffer[%X]\n", reg - HEAD0, block_start);
				context->regs[reg] =context->buffer[block_start++];
				block_start &= (sizeof(context->buffer)-1);
			}
			printf("Decode done %X:%X:%X mode %X\n", context->regs[HEAD0], context->regs[HEAD1], context->regs[HEAD2], context->regs[HEAD3]);
		}
		if (context->transfer_end != CYCLE_NEVER) {
			if (context->byte_handler(context->handler_data, context->buffer[context->dac & (sizeof(context->buffer)-1)])) {
				context->dac++;
				context->regs[DBCL]--;
				if (context->regs[DBCL] == 0xFF) {
					context->regs[DBCH]--;
					if (context->regs[DBCH] == 0xFF) {
						context->regs[IFSTAT] &= ~BIT_DTEI;
						context->regs[IFSTAT] |= BIT_DTBSY;
						if (context->cycle != context->transfer_end) {
							printf("Expected transfer end at %u but ended at %u\n", context->transfer_end, context->cycle);
						}
						context->transfer_end = CYCLE_NEVER;
					}
				}
			} else {
				// pause transfer
				context->transfer_end = CYCLE_NEVER;
			}
		}
	}
}

void lc8951_resume_transfer(lc8951 *context)
{
	if (context->transfer_end == CYCLE_NEVER && (context->ifctrl & BIT_DOUTEN)) {
		uint16_t transfer_size = context->regs[DBCL] | (context->regs[DBCH] << 8);
		if (transfer_size) {
			context->transfer_end = context->cycle + transfer_size * context->clock_step;
			printf("RESUME: size %u, cycle %u, end %u\n", transfer_size, context->cycle, context->transfer_end);
		}
	}
}

void lc8951_write_byte(lc8951 *context, uint32_t cycle, int sector_offset, uint8_t byte)
{
	lc8951_run(context, cycle);
	uint16_t current_write_addr = context->regs[WAL] | (context->regs[WAH] << 8);
	if (sector_offset == 12) {
		//we've recevied the sync pattern for the next block

		//header/status regs no longer considered "valid"
		context->regs[STAT3] |= BIT_VALST;
		if ((context->ctrl0 & (BIT_DECEN|BIT_WRRQ)) == (BIT_DECEN|BIT_WRRQ)) {
			uint16_t block_start = current_write_addr - 2352;
			context->regs[PTL] = block_start;
			context->regs[PTH] = block_start >> 8;
			printf("Decoding block starting at %X\n", block_start);
			context->ptl_internal = block_start & (sizeof(context->buffer)-1);
			context->decode_end = context->cycle + 2352 * context->clock_step * 4;
		}
	}
	if (sector_offset >= 12 && sector_offset < 16) {
		//TODO: Handle SHDREN = 1
		if ((context->ctrl0 & (BIT_DECEN|BIT_WRRQ)) == (BIT_DECEN)) {
			//monitor only mode
			context->regs[HEAD0 + sector_offset - 12] = byte;
		}
	}
	if ((context->ctrl0 & (BIT_DECEN|BIT_WRRQ)) == (BIT_DECEN|BIT_WRRQ)) {
		context->buffer[current_write_addr & (sizeof(context->buffer)-1)] = byte;
		context->regs[WAL]++;
		if (!context->regs[WAL]) {
			context->regs[WAH]++;
		}
	}
}

uint32_t lc8951_next_interrupt(lc8951 *context)
{
	if ((~context->regs[IFSTAT]) & context->ifctrl & (BIT_CMDI|BIT_DTEI|BIT_DECI)) {
		//interrupt already pending
		return context->cycle;
	}
	uint32_t deci_cycle = CYCLE_NEVER;
	if (context->ifctrl & BIT_DECI) {
		deci_cycle = context->decode_end;
	}
	uint32_t dtei_cycle = CYCLE_NEVER;
	if (context->ifctrl & BIT_DTEI) {
		dtei_cycle = context->transfer_end;
	}
	return deci_cycle < dtei_cycle ? deci_cycle : dtei_cycle;
}
