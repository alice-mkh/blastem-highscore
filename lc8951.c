#include "lc8951.h"

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
#define BIT_CECIEN 0x20
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

//datasheet timing info
//3 cycles for memory operation
//6 cycles min for DMA-mode host transfer

void lc8951_init(lc8951 *context)
{
	//This seems to vary somewhat between Sega CD models
	//unclear if the difference is in the lc8951 or gate array
	context->regs[IFSTAT] = 0xFF;
	context->ar_mask = 0x1F;
}

void lc8951_reg_write(lc8951 *context, uint8_t value)
{
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
		if (value & BIT_DOUTEN) {
			context->regs[IFSTAT] &= ~BIT_DTBSY;
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
	case PTL_WRITE:
		context->regs[PTL] = value;
		break;
	case PTH_WRITE:
		context->regs[PTH] = value;
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
	context->ar++;
	context->ar &= context->ar_mask;
	return value;
}

void lc8951_ar_write(lc8951 *context, uint8_t value)
{
	context->ar = value & context->ar_mask;
}
