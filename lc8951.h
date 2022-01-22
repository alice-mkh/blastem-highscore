#ifndef LC8951_H_
#define LC8951_H_

#include <stdint.h>

typedef struct {
	uint32_t cycles;

	uint8_t buffer[0x4000];

	uint8_t regs[16];
	uint8_t comin[8];

	uint16_t dac;
	uint8_t comin_write;
	uint8_t comin_count;
	uint8_t ifctrl;
	uint8_t ctrl0;
	uint8_t ctrl1;
	uint8_t ar;
	uint8_t ar_mask;
} lc8951;

void lc8951_init(lc8951 *context);
//void lc8951_run(lc8951 *context, uint32_t cycle);
void lc8951_reg_write(lc8951 *context, uint8_t value);
uint8_t lc8951_reg_read(lc8951 *context);
void lc8951_ar_write(lc8951 *context, uint8_t value);

#endif //LC8951_H_
