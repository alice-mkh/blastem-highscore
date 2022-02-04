/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "render.h"
#include "ym2612.h"
#include "psg.h"
#include "config.h"
#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vgm.h"
#include "system.h"
#include "rf5c164.h"

#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

#define MCLKS_PER_68K 7
#define MCLKS_PER_YM  MCLKS_PER_68K
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)


#ifdef DISABLE_ZLIB
#define VGMFILE FILE*
#define vgmopen fopen
#define vgmread fread
#define vgmseek fseek
#define vgmgetc fgetc
#define vgmclose fclose
#else
#include "zlib/zlib.h"
#define VGMFILE gzFile
#define vgmopen gzopen
#define vgmread gzfread
#define vgmseek gzseek
#define vgmgetc gzgetc
#define vgmclose gzclose
#endif


system_header *current_system;

void system_request_exit(system_header *system, uint8_t force_release)
{
}

void handle_keydown(int keycode)
{
}

void handle_keyup(int keycode)
{
}

void handle_joydown(int joystick, int button)
{
}

void handle_joyup(int joystick, int button)
{
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
}

void handle_joy_axis(int joystick, int axis, int16_t value)
{
}

void handle_joy_added(int joystick)
{
}

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
}

void handle_mousedown(int mouse, int button)
{
}

void handle_mouseup(int mouse, int button)
{
}

int headless = 0;

#include <limits.h>
#define ADJUST_BUFFER (12500000)
#define MAX_NO_ADJUST (UINT_MAX-ADJUST_BUFFER)
#define MAX_RUN_SAMPLES 128
tern_node * config;

typedef struct chip_info chip_info;
typedef void (*run_fun)(void *context, uint32_t cycle);
typedef void (*adjust_fun)(chip_info *chip);
struct chip_info {
	void       *context;
	run_fun    run;
	adjust_fun adjust;
	uint32_t   clock;
	uint32_t   samples;
};

uint32_t cycles_to_samples(uint32_t clock_rate, uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)44100) / ((uint64_t)clock_rate);
}

uint32_t samples_to_cycles(uint32_t clock_rate, uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)clock_rate) / ((uint64_t)44100);
}

void ym_adjust(chip_info *chip)
{
	ym2612_context *ym = chip->context;
	if (ym->current_cycle >= MAX_NO_ADJUST) {
		uint32_t deduction = ym->current_cycle - ADJUST_BUFFER;
		chip->samples -= cycles_to_samples(chip->clock, deduction);
		ym->current_cycle -= deduction;
	}
}

void psg_adjust(chip_info *chip)
{
	psg_context *psg = chip->context;
	if (psg->cycles >= MAX_NO_ADJUST) {
		uint32_t deduction = psg->cycles - ADJUST_BUFFER;
		chip->samples -= cycles_to_samples(chip->clock, deduction);
		psg->cycles -= deduction;
	}
}

void pcm_adjust(chip_info *chip)
{
	rf5c164 *pcm = chip->context;
	if (pcm->cycle >= MAX_NO_ADJUST) {
		uint32_t deduction = pcm->cycle - ADJUST_BUFFER;
		chip->samples -= cycles_to_samples(chip->clock, deduction);
		pcm->cycle -= deduction;
	}
}

void vgm_wait(chip_info *chips, uint32_t num_chips, uint32_t *completed_samples, uint32_t samples)
{
	while (samples > MAX_RUN_SAMPLES)
	{
		vgm_wait(chips, num_chips, completed_samples, MAX_RUN_SAMPLES);
		samples -= MAX_RUN_SAMPLES;
	}
	*completed_samples += samples;
	for (uint32_t i = 0; i < num_chips; i++)
	{
		chips[i].samples += samples;
		chips[i].run(chips[i].context, samples_to_cycles(chips[i].clock, chips[i].samples));
		chips[i].adjust(chips + i);
	}
	if (*completed_samples > 44100/60) {
		process_events();
	}
}

chip_info chips[64];

uint8_t *find_block(data_block *head, uint32_t offset, uint32_t size)
{
	if (!head) {
		return NULL;
	}
	while (head->size < offset) {
		offset -= head->size;
		head = head->next;
	}
	if (head->size - offset < size) {
		return NULL;
	}
	return head->data + offset;
}

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
	data_block *blocks = NULL;
	data_block *seek_block = NULL;
	data_block *pcm68_blocks = NULL;
	data_block *pcm164_blocks = NULL;
	uint32_t seek_offset;
	uint32_t block_offset;

	uint32_t fps = 60;
	config = load_config(argv[0]);
	render_init(320, 240, "vgm play", 0);

	uint32_t opts = 0;
	if (argc >= 3 && !strcmp(argv[2], "-y")) {
		opts |= YM_OPT_WAVE_LOG;
	}

	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0", TVAL_PTR).ptrval;
	uint32_t lowpass_cutoff = lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : 3390;

	VGMFILE f = vgmopen(argv[1], "rb");
	vgm_header header;
	vgmread(&header, sizeof(header), 1, f);
	if (header.version < 0x150 || !header.data_offset) {
		header.data_offset = 0xC;
	}
	if (header.version <= 0x101) {
		header.ym2612_clk = header.ym2413_clk;
	}
	uint32_t num_chips = 0;
	rf5c164 *pcm68 = NULL;
	if ((header.data_offset + 0x34) >= 0x44) {
		uint32_t pcm68_clock = 0;
		vgmseek(f, 0x40, SEEK_SET);
		vgmread(&pcm68_clock, sizeof(pcm68_clock), 1, f);
		if (pcm68_clock) {
			pcm68 = calloc(sizeof(*pcm68), 1);
			rf5c164_init(pcm68, pcm68_clock, 1);
			chips[num_chips++] = (chip_info) {
				.context = pcm68,
				.run = (run_fun)rf5c164_run,
				.adjust = pcm_adjust,
				.clock = pcm68_clock,
				.samples = 0
			};
		}
	}
	rf5c164 *pcm164 = NULL;
	if ((header.data_offset + 0x34) >= 0x70) {
		uint32_t pcm164_clock = 0;
		vgmseek(f, 0x6C, SEEK_SET);
		vgmread(&pcm164_clock, sizeof(pcm164_clock), 1, f);
		if (pcm164_clock) {
			pcm164 = calloc(sizeof(*pcm164), 1);
			rf5c164_init(pcm164, pcm164_clock, 1);
			chips[num_chips++] = (chip_info) {
				.context = pcm164,
				.run = (run_fun)rf5c164_run,
				.adjust = pcm_adjust,
				.clock = pcm164_clock,
				.samples = 0
			};
		}
	}

	ym2612_context y_context;
	if (header.ym2612_clk) {
		ym_init(&y_context, header.ym2612_clk, 1, opts);
		chips[num_chips++] = (chip_info) {
			.context = &y_context,
			.run = (run_fun)ym_run,
			.adjust = ym_adjust,
			.clock = header.ym2612_clk,
			.samples = 0
		};
	}

	psg_context p_context;
	if (header.sn76489_clk) {
		psg_init(&p_context, header.sn76489_clk, 1);
		chips[num_chips++] = (chip_info) {
			.context = &p_context,
			.run = (run_fun)psg_run,
			.adjust = psg_adjust,
			.clock = header.sn76489_clk,
			.samples = 0
		};
	}

	vgmseek(f, header.data_offset + 0x34, SEEK_SET);
	uint32_t data_size = header.eof_offset + 4 - (header.data_offset + 0x34);
	uint8_t * data = malloc(data_size);
	vgmread(data, 1, data_size, f);
	vgmclose(f);

	uint32_t loop_count = 2;

	uint8_t * end = data + data_size;
	uint8_t * cur = data;
	uint32_t completed_samples = 0;
	while (cur < end) {
		uint8_t cmd = *(cur++);
		switch(cmd)
		{
		case CMD_PSG_STEREO:
			//ignore for now
			cur++;
			break;
		case CMD_PSG:
			psg_write(&p_context, *(cur++));
			break;
		case CMD_YM2612_0:
			ym_address_write_part1(&y_context, *(cur++));
			ym_data_write(&y_context, *(cur++));
			break;
		case CMD_YM2612_1:
			ym_address_write_part2(&y_context, *(cur++));
			ym_data_write(&y_context, *(cur++));
			break;
		case CMD_WAIT: {
			uint32_t wait_time = *(cur++);
			wait_time |= *(cur++) << 8;
			vgm_wait(chips, num_chips, &completed_samples, wait_time);
			break;
		}
		case CMD_WAIT_60:
			vgm_wait(chips, num_chips, &completed_samples, 735);
			break;
		case CMD_WAIT_50:
			vgm_wait(chips, num_chips, &completed_samples, 882);
			break;
		case CMD_END:
			if (header.loop_offset && --loop_count) {
				cur = data + header.loop_offset + 0x1C - (header.data_offset + 0x34);
			} else {
				//TODO: fade out
				return 0;
			}
			break;
		case CMD_PCM_WRITE:
			if (end - cur < 11) {
				fatal_error("early end of stream at offset %X\n", data-cur);
			}
			cur++;
			{
				uint8_t chip_type = *(cur++);
				uint32_t read_offset = *(cur++);
				read_offset |= *(cur++) << 8;
				read_offset |= *(cur++) << 16;
				uint32_t write_offset = *(cur++);
				write_offset |= *(cur++) << 8;
				write_offset |= *(cur++) << 16;
				uint32_t size = *(cur++);
				size |= *(cur++) << 8;
				size |= *(cur++) << 16;
				if (chip_type == DATA_RF5C68) {
					uint8_t *src = find_block(pcm68_blocks, read_offset, size);
					if (!src) {
						printf("Failed to find RF6C68 data offset %X with size %X\n", read_offset, size);
					}
					write_offset |= pcm68->ram_bank;
					write_offset &= 0xFFFF;
					if (size + write_offset > 0x10000) {
						size = 0x10000 - write_offset;
					}
					memcpy(pcm68->ram + write_offset, src, size);
					printf("rf5c68 PCM write read_offset %X, write_offset %X, size %X\n", read_offset, write_offset, size);
				} else if (chip_type == DATA_RF5C164) {
					uint8_t *src = find_block(pcm164_blocks, read_offset, size);
					if (!src) {
						printf("Failed to find RF6C68 data offset %X with size %X\n", read_offset, size);
					}
					write_offset |= pcm164->ram_bank;
					write_offset &= 0xFFFF;
					if (size + write_offset > 0x10000) {
						size = 0x10000 - write_offset;
					}
					memcpy(pcm164->ram + write_offset, src, size);
					printf("rf5c164 PCM write read_offset %X, write_offset %X, size %X\n", read_offset, write_offset, size);
				} else {
					printf("unknown PCM write read_offset %X, write_offset %X, size %X\n", read_offset, write_offset, size);
				}
			}
			break;
		case CMD_PCM68_REG:
			if (pcm68) {
				uint8_t reg = *(cur++);
				uint8_t value = *(cur++);

				rf5c164_write(pcm68, reg, value);
			} else {
				printf("CMD_PCM68_REG without rf5c68 clock\n");
				cur += 2;
			}
			break;
		case CMD_PCM164_REG:
			if (pcm164) {
				uint8_t reg = *(cur++);
				uint8_t value = *(cur++);
				rf5c164_write(pcm164, reg, value);
			} else {
				printf("CMD_PCM164_REG without rf5c164 clock\n");
				cur += 2;
			}
			break;
		case CMD_PCM68_RAM:
			if (pcm68) {
				uint16_t address = *(cur++);
				address |= *(cur++) << 8;
				address &= 0xFFF;
				address |= 0x1000;
				uint8_t value = *(cur++);
				rf5c164_write(pcm68, address, value);
			}
			break;
		case CMD_PCM164_RAM:
			if (pcm164) {
				uint16_t address = *(cur++);
				address |= *(cur++) << 8;
				address &= 0xFFF;
				address |= 0x1000;
				uint8_t value = *(cur++);
				rf5c164_write(pcm164, address, value);
			}
			break;
		case CMD_DATA: {
			cur++; //skip compat command
			uint8_t data_type = *(cur++);
			uint32_t data_size = *(cur++);
			data_size |= *(cur++) << 8;
			data_size |= *(cur++) << 16;
			data_size |= *(cur++) << 24;
			data_block **curblock = NULL;
			if (data_type == DATA_YM2612_PCM) {
				curblock = &blocks;
			} else if (data_type == DATA_RF5C68) {
				curblock = &pcm68_blocks;
			} else if (data_type == DATA_RF5C164) {
				curblock = &pcm164_blocks;
			}

			if (curblock) {
				while(*curblock)
				{
					curblock = &((*curblock)->next);
				}
				*curblock = malloc(sizeof(data_block));
				(*curblock)->size = data_size;
				(*curblock)->type = data_type;
				(*curblock)->data = cur;
				(*curblock)->next = NULL;
			} else {
				fprintf(stderr, "Skipping data block with unrecognized type %X\n", data_type);
			}
			cur += data_size;
			break;
		}
		case CMD_DATA_SEEK: {
			uint32_t new_offset = *(cur++);
			new_offset |= *(cur++) << 8;
			new_offset |= *(cur++) << 16;
			new_offset |= *(cur++) << 24;
			if (!seek_block || new_offset < seek_offset) {
				seek_block = blocks;
				seek_offset = 0;
				block_offset = 0;
			}
			while (seek_block && (seek_offset - block_offset + seek_block->size) < new_offset)
			{
				seek_offset += seek_block->size - block_offset;
				seek_block = seek_block->next;
				block_offset = 0;
			}
			block_offset += new_offset-seek_offset;
			seek_offset = new_offset;
			break;
		}

		default:
			if (cmd >= CMD_WAIT_SHORT && cmd < (CMD_WAIT_SHORT + 0x10)) {
				uint32_t wait_time = (cmd & 0xF) + 1;
				vgm_wait(chips, num_chips, &completed_samples, wait_time);
			} else if (cmd >= CMD_YM2612_DAC && cmd < CMD_DAC_STREAM_SETUP) {
				if (seek_block) {
					ym_address_write_part1(&y_context, 0x2A);
					ym_data_write(&y_context, seek_block->data[block_offset++]);
					seek_offset++;
					if (block_offset > seek_block->size) {
						seek_block = seek_block->next;
						block_offset = 0;
					}
				} else {
					fputs("Encountered DAC write command but data seek pointer is invalid!\n", stderr);
				}
				uint32_t wait_time = (cmd & 0xF);
				if (wait_time)
				{
					vgm_wait(chips, num_chips, &completed_samples, wait_time);
				}
			} else {
				fatal_error("unimplemented command: %X at offset %X\n", cmd, (unsigned int)(cur - data - 1));
			}
		}
	}
	return 0;
}
