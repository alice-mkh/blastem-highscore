#include <stdlib.h>
#include <string.h>
#include "ymf262.h"
#include "render_audio.h"

void ymf262_init(ymf262_context *context, uint32_t master_clock, uint32_t clock_div, uint32_t options)
{
	memset(context, 0, sizeof(*context));
	context->clock_inc = clock_div * 8;
	context->audio = render_audio_source("YMF262", master_clock, context->clock_inc * OPL3_NUM_OPERATORS, 2);
	ymf262_reset(context);
}

void ymf262_reset(ymf262_context *context)
{
	//TODO: implement me
}

void ymf262_free(ymf262_context *context)
{
	render_free_source(context->audio);
	free(context);
}

void ymf262_adjust_master_clock(ymf262_context *context, uint32_t master_clock)
{
	render_audio_adjust_clock(context->audio, master_clock, context->clock_inc * OPL3_NUM_OPERATORS);
}

void ymf262_adjust_cycles(ymf262_context *context, uint32_t deduction)
{
	context->cycle -= deduction;
}

void ymf262_run(ymf262_context *context, uint32_t to_cycle)
{
	for (; context->cycle < to_cycle; context->cycle += context->clock_inc)
	{
		context->current_op++;
		if (context->current_op == OPL3_NUM_OPERATORS) {
			context->current_op = 0;
			int16_t left = 0, right = 0;
			render_put_stereo_sample(context->audio, left, right);
		}
	}
}

void ymf262_address_write_part1(ymf262_context *context, uint8_t address)
{
	context->selected_reg = address;
	context->selected_part = 0;
}
void ymf262_address_write_part2(ymf262_context *context, uint8_t address)
{
	context->selected_reg = address;
	context->selected_part = 0;
}

#define OPL3_NTS 0x08

void ymf262_data_write(ymf262_context *context, uint8_t value)
{
	if (!context->selected_reg) {
		return;
	}
	uint8_t old = 0;
	if (context->selected_reg >= OPL3_PARAM_START && context->selected_reg < OPL3_PARAM_END) {
		if (context->selected_part) {
			old = context->part2_regs[context->selected_reg - OPL3_PARAM_START];
			context->part2_regs[context->selected_reg - OPL3_PARAM_START] = value;
		} else {
			old = context->part1_regs[context->selected_reg - OPL3_PARAM_START];
			context->part1_regs[context->selected_reg - OPL3_PARAM_START] = value;
		}
	} else if (context->selected_part) {
		if (context->selected_reg <= sizeof(context->timer_test)) {
			old = context->timer_test[context->selected_reg - 1];
			context->timer_test[context->selected_reg - 1] = value;
		} else if (context->selected_reg == OPL3_NTS) {
			old = context->nts;
			context->nts = value;
		} else {
			return;
		}
	} else {
		switch (context->selected_reg)
		{
		case 0x01:
			old = context->part2_test;
			context->part2_test = value;
			break;
		case 0x04:
			old = context->connection_sel;
			context->connection_sel = value;
			break;
		case 0x05:
			old = context->opl3_mode;
			context->opl3_mode = value;
			break;
		default:
			return;
		}
	}
	if (value != old) {
		if (context->vgm) {
			if (context->selected_reg) {
				vgm_ymf262_part2_write(context->vgm, context->cycle, context->selected_reg, value);
			} else {
				vgm_ymf262_part1_write(context->vgm, context->cycle, context->selected_reg, value);
			}
		}
	}
}

void ymf262_vgm_log(ymf262_context *context, uint32_t master_clock, vgm_writer *vgm)
{
	vgm_ymf262_init(vgm, 8 * master_clock / context->clock_inc);
	context->vgm = vgm;
	//TODO: write initial state
}

uint8_t ymf262_read_status(ymf262_context *context, uint32_t cycle, uint32_t port)
{
	if (port) {
		//TODO: Investigate behavior of invalid status reads
		return 0xFF;
	}
	return context->status;
}
