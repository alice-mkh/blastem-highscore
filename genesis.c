/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "genesis.h"
#include "segacd.h"
#include "blastem.h"
#include "nor.h"
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include "render.h"
#include "gst.h"
#include "util.h"
#include "debug.h"
#include "gdb_remote.h"
#include "saves.h"
#include "bindings.h"
#include "jcart.h"
#include "config.h"
#include "event_log.h"
#include "paths.h"
#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

uint32_t MCLKS_PER_68K;
#define MCLKS_PER_YM  7
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)
#define Z80_INT_PULSE_MCLKS 2573 //measured value is ~171.5 Z80 clocks
#define DEFAULT_SYNC_INTERVAL MCLKS_LINE
#define DEFAULT_LOWPASS_CUTOFF 3390

//TODO: Figure out the exact value for this
#define LINES_NTSC 262
#define LINES_PAL 313

#ifdef IS_LIB
#define MAX_SOUND_CYCLES (MCLKS_PER_YM*NUM_OPERATORS*6*4)
#else
#define MAX_SOUND_CYCLES 100000
#endif

#ifdef NEW_CORE
#define Z80_CYCLE cycles
#define Z80_OPTS opts
#define z80_handle_code_write(...)
#else
#define Z80_CYCLE current_cycle
#define Z80_OPTS options
#endif

void genesis_serialize(genesis_context *gen, serialize_buffer *buf, uint32_t m68k_pc, uint8_t all)
{
	if (all) {
		start_section(buf, SECTION_68000);
		m68k_serialize(gen->m68k, m68k_pc, buf);
		end_section(buf);

		start_section(buf, SECTION_Z80);
		z80_serialize(gen->z80, buf);
		end_section(buf);
	}

	start_section(buf, SECTION_VDP);
	vdp_serialize(gen->vdp, buf);
	end_section(buf);

	start_section(buf, SECTION_YM2612);
	ym_serialize(gen->ym, buf);
	end_section(buf);

	start_section(buf, SECTION_PSG);
	psg_serialize(gen->psg, buf);
	end_section(buf);

	if (all) {
		start_section(buf, SECTION_GEN_BUS_ARBITER);
		save_int8(buf, gen->z80->reset);
		save_int8(buf, gen->z80->busreq);
		save_int16(buf, gen->z80_bank_reg);
		//I think the refresh logic may technically be part of the VDP, but whatever
		save_int32(buf, gen->last_sync_cycle);
		save_int32(buf, gen->refresh_counter);
		end_section(buf);

		start_section(buf, SECTION_SEGA_IO_1);
		io_serialize(gen->io.ports, buf);
		end_section(buf);

		start_section(buf, SECTION_SEGA_IO_2);
		io_serialize(gen->io.ports + 1, buf);
		end_section(buf);

		start_section(buf, SECTION_SEGA_IO_EXT);
		io_serialize(gen->io.ports + 2, buf);
		end_section(buf);

		start_section(buf, SECTION_MAIN_RAM);
		save_int8(buf, RAM_WORDS * 2 / 1024);
		save_buffer16(buf, gen->work_ram, RAM_WORDS);
		end_section(buf);

		start_section(buf, SECTION_SOUND_RAM);
		save_int8(buf, Z80_RAM_BYTES / 1024);
		save_buffer8(buf, gen->zram, Z80_RAM_BYTES);
		end_section(buf);

		if (gen->version_reg & 0xF) {
			//only save TMSS info if it's present
			//that will allow a state saved on a model lacking TMSS
			//to be loaded on a model that has it
			start_section(buf, SECTION_TMSS);
			save_int8(buf, gen->tmss);
			save_buffer16(buf, gen->tmss_lock, 2);
			end_section(buf);
		}

		cart_serialize(&gen->header, buf);
	}
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_serialize(cd, buf, all);
	}
}

static uint8_t *serialize(system_header *sys, size_t *size_out)
{
	genesis_context *gen = (genesis_context *)sys;
	uint32_t address;
	if (gen->m68k->resume_pc) {
		gen->m68k->target_cycle = gen->m68k->current_cycle;
		gen->header.save_state = SERIALIZE_SLOT+1;
		resume_68k(gen->m68k);
		if (size_out) {
			*size_out = gen->serialize_size;
		}
		return gen->serialize_tmp;
	} else {
		serialize_buffer state;
		init_serialize(&state);
		uint32_t address = read_word(4, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen, gen->m68k) << 16;
		address |= read_word(6, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen, gen->m68k);
		genesis_serialize(gen, &state, address, 1);
		if (size_out) {
			*size_out = state.size;
		}
		return state.data;
	}
}

static void ram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024 / 2;
	if (ram_size > RAM_WORDS) {
		fatal_error("State has a RAM size of %d bytes", ram_size * 2);
	}
	load_buffer16(buf, gen->work_ram, ram_size);
	m68k_invalidate_code_range(gen->m68k, 0xE00000, 0x1000000);
}

static void zram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > Z80_RAM_BYTES) {
		fatal_error("State has a Z80 RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, gen->zram, ram_size);
	z80_invalidate_code_range(gen->z80, 0, 0x4000);
}

static void update_z80_bank_pointer(genesis_context *gen)
{
	if (gen->z80_bank_reg < 0x140) {
		gen->z80->mem_pointers[1] = get_native_pointer(gen->z80_bank_reg << 15, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen);
	} else {
		gen->z80->mem_pointers[1] = NULL;
	}
	z80_invalidate_code_range(gen->z80, 0x8000, 0xFFFF);
}

static void bus_arbiter_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	gen->z80->reset = load_int8(buf);
	gen->z80->busreq = load_int8(buf);
	gen->z80_bank_reg = load_int16(buf) & 0x1FF;
	if ((buf->size - buf->cur_pos) >= 2 * sizeof(uint32_t)) {
		gen->last_sync_cycle = load_int32(buf);
		gen->refresh_counter = load_int32(buf);
	} else {
		//save state is from an older version of BlastEm that lacks these fields
		//set them to reasonable values
		gen->last_sync_cycle = gen->m68k->current_cycle;
		gen->refresh_counter = 0;
	}
}

static void tmss_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	gen->tmss = load_int8(buf);
	load_buffer16(buf, gen->tmss_lock, 2);
}

static void adjust_int_cycle(m68k_context * context, vdp_context * v_context);
static void check_tmss_lock(genesis_context *gen);
static void toggle_tmss_rom(genesis_context *gen);
#include "m68k_internal.h" //needed for get_native_address_trans, should be eliminated once handling of PC is cleaned up
void genesis_deserialize(deserialize_buffer *buf, genesis_context *gen)
{
	register_section_handler(buf, (section_handler){.fun = m68k_deserialize, .data = gen->m68k}, SECTION_68000);
	register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = gen->z80}, SECTION_Z80);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = gen->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = ym_deserialize, .data = gen->ym}, SECTION_YM2612);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = gen->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = bus_arbiter_deserialize, .data = gen}, SECTION_GEN_BUS_ARBITER);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports}, SECTION_SEGA_IO_1);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 1}, SECTION_SEGA_IO_2);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 2}, SECTION_SEGA_IO_EXT);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = gen}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = zram_deserialize, .data = gen}, SECTION_SOUND_RAM);
	register_section_handler(buf, (section_handler){.fun = cart_deserialize, .data = gen}, SECTION_MAPPER);
	register_section_handler(buf, (section_handler){.fun = tmss_deserialize, .data = gen}, SECTION_TMSS);
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_register_section_handlers(cd, buf);
	}
	uint8_t tmss_old = gen->tmss;
	gen->tmss = 0xFF;
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	if (gen->version_reg & 0xF) {
		if (gen->tmss == 0xFF) {
			//state lacked a TMSS section, assume that the game ROM is mapped in
			//and that the VDP is unlocked
			gen->tmss_lock[0] = 0x5345;
			gen->tmss_lock[1] = 0x4741;
			gen->tmss = 1;
		}
		if (gen->tmss != tmss_old) {
			toggle_tmss_rom(gen);
		}
		check_tmss_lock(gen);
	}
	update_z80_bank_pointer(gen);
	adjust_int_cycle(gen->m68k, gen->vdp);
	//HACK: Fix this once PC/IR is represented in a better way in 68K core
	//Would be better for this hack to live in side the 68K core itself, but it's better to do it
	//after RAM has been loaded to avoid any unnecessary retranslation
	gen->m68k->resume_pc = get_native_address_trans(gen->m68k, gen->m68k->last_prefetch_address);
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		cd->m68k->resume_pc = get_native_address_trans(cd->m68k, cd->m68k->last_prefetch_address);
	}
	free(buf->handlers);
	buf->handlers = NULL;
}

static void deserialize(system_header *sys, uint8_t *data, size_t size)
{
	genesis_context *gen = (genesis_context *)sys;
	deserialize_buffer buffer;
	init_deserialize(&buffer, data, size);
	genesis_deserialize(&buffer, gen);
}

uint16_t read_dma_value(uint32_t address)
{
	genesis_context *genesis = (genesis_context *)current_system;
	address *= 2;
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	if ((address >= 0xA00000 && address < 0xB00000) || (address >= 0xC00000 && address <= 0xE00000)) {
		return 0;
	}
	if (genesis->expansion) {
		segacd_context *cd = genesis->expansion;
		uint32_t word_ram = cd->base + 0x200000;
		uint32_t word_ram_end = cd->base + 0x240000;
		if (address >= word_ram && address < word_ram_end) {
			//FIXME: first word should just be garbage
			address -= 2;
		}
	}

	return read_word(address, (void **)genesis->m68k->mem_pointers, &genesis->m68k->options->gen, genesis->m68k);
}

static uint16_t get_open_bus_value(system_header *system)
{
	genesis_context *genesis = (genesis_context *)system;
	return read_dma_value(genesis->m68k->last_prefetch_address/2);
}

static void adjust_int_cycle(m68k_context * context, vdp_context * v_context)
{
	//static int old_int_cycle = CYCLE_NEVER;
	genesis_context *gen = context->system;
	if (context->sync_cycle - context->current_cycle > gen->max_cycles) {
		context->sync_cycle = context->current_cycle + gen->max_cycles;
	}
	context->int_cycle = CYCLE_NEVER;
	uint8_t mask = context->status & 0x7;
	if (mask < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if (mask < 4) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				next_hint = next_hint < context->current_cycle ? context->current_cycle : next_hint;
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 4;

				}
			}
			if (mask < 2 && (v_context->regs[REG_MODE_3] & BIT_EINT_EN)) {
				uint32_t next_eint_port0 = io_next_interrupt(gen->io.ports, context->current_cycle);
				uint32_t next_eint_port1 = io_next_interrupt(gen->io.ports + 1, context->current_cycle);
				uint32_t next_eint_port2 = io_next_interrupt(gen->io.ports + 2, context->current_cycle);
				uint32_t next_eint = next_eint_port0 < next_eint_port1
					? (next_eint_port0 < next_eint_port2 ? next_eint_port0 : next_eint_port2)
					: (next_eint_port1 < next_eint_port2 ? next_eint_port1 : next_eint_port2);
				if (next_eint != CYCLE_NEVER) {
					next_eint = next_eint < context->current_cycle ? context->current_cycle : next_eint;
					if (next_eint < context->int_cycle) {
						context->int_cycle = next_eint;
						context->int_num = 2;
					}
				}
			}
		}
	}
	if (context->int_cycle > context->current_cycle && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	/*if (context->int_cycle != old_int_cycle) {
		printf("int cycle changed to: %d, level: %d @ %d(%d), frame: %d, vcounter: %d, hslot: %d, mask: %d, hint_counter: %d\n", context->int_cycle, context->int_num, v_context->cycles, context->current_cycle, v_context->frame, v_context->vcounter, v_context->hslot, context->status & 0x7, v_context->hint_counter);
		old_int_cycle = context->int_cycle;
	}*/

	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->current_cycle;
		return;
	}

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	if (context->should_return || gen->header.enter_debugger) {
		context->target_cycle = context->current_cycle;
	} else if (context->target_cycle < context->current_cycle) {
		//Changes to SR can result in an interrupt cycle that's in the past
		//This can cause issues with the implementation of STOP though
		context->target_cycle = context->current_cycle;
	}
	if (context->target_cycle == context->int_cycle) {
		//Currently delays from Z80 access and refresh are applied only when we sync
		//this can cause extra latency when it comes to interrupts
		//to prevent this code forces some extra synchronization in the period immediately before an interrupt
		if ((context->target_cycle - context->current_cycle) > gen->int_latency_prev1) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev1;
		} else if ((context->target_cycle - context->current_cycle) > gen->int_latency_prev2) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev2;
		} else {
			context->target_cycle = context->sync_cycle = context->current_cycle;
		}

	}
	/*printf("Cyc: %d, Trgt: %d, Int Cyc: %d, Int: %d, Mask: %X, V: %d, H: %d, HICount: %d, HReg: %d, Line: %d\n",
		context->current_cycle, context->target_cycle, context->int_cycle, context->int_num, (context->status & 0x7),
		v_context->regs[REG_MODE_2] & 0x20, v_context->regs[REG_MODE_1] & 0x10, v_context->hint_counter, v_context->regs[REG_HINT], v_context->cycles / MCLKS_LINE);*/
}

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

static void z80_next_int_pulse(z80_context * z_context)
{
	genesis_context * gen = z_context->system;
#ifdef NEW_CORE
	z_context->int_cycle = vdp_next_vint_z80(gen->vdp);
	z_context->int_end_cycle = z_context->int_cycle + Z80_INT_PULSE_MCLKS;
	z_context->int_value = 0xFF;
	z80_sync_cycle(z_context, z_context->sync_cycle);
#else
	z_context->int_pulse_start = vdp_next_vint_z80(gen->vdp);
	z_context->int_pulse_end = z_context->int_pulse_start + Z80_INT_PULSE_MCLKS;
	z_context->im2_vector = 0xFF;
#endif
}

static void sync_z80(genesis_context *gen, uint32_t mclks)
{
	z80_context *z_context = gen->z80;
#ifndef NO_Z80
	if (z80_enabled) {
#ifdef NEW_CORE
		if (z_context->int_cycle == 0xFFFFFFFFU) {
			z80_next_int_pulse(z_context);
		}
#endif
		if (gen->enter_z80_debugger && !z_context->reset && !z_context->busack) {
			while (!z_context->pc) {
				z80_run(z_context, z_context->current_cycle + 4);
			}
			gen->enter_z80_debugger = 0;
#ifndef IS_LIB
			zdebugger(z_context, z_context->pc);
#endif
		}
		z80_run(z_context, mclks);
	} else
#endif
	{
		z_context->Z80_CYCLE = mclks;
	}
}

static void sync_sound(genesis_context * gen, uint32_t target)
{
	//printf("YM | Cycle: %d, bpos: %d, PSG | Cycle: %d, bpos: %d\n", gen->ym->current_cycle, gen->ym->buffer_pos, gen->psg->cycles, gen->psg->buffer_pos * 2);
	while (target > gen->psg->cycles && target - gen->psg->cycles > MAX_SOUND_CYCLES) {
		uint32_t cur_target = gen->psg->cycles + MAX_SOUND_CYCLES;
		//printf("Running PSG to cycle %d\n", cur_target);
		psg_run(gen->psg, cur_target);
		//printf("Running YM-2612 to cycle %d\n", cur_target);
		ym_run(gen->ym, cur_target);
		if (gen->expansion) {
			scd_run(gen->expansion, gen_cycle_to_scd(cur_target, gen));
		}
	}
	psg_run(gen->psg, target);
	ym_run(gen->ym, target);
	if (gen->expansion) {
		scd_run(gen->expansion, gen_cycle_to_scd(target, gen));
	}

	//printf("Target: %d, YM bufferpos: %d, PSG bufferpos: %d\n", target, gen->ym->buffer_pos, gen->psg->buffer_pos * 2);
}

#define REFRESH_INTERVAL 128
#define REFRESH_DELAY 2

#include <limits.h>
#define ADJUST_BUFFER (8*MCLKS_LINE*313)
#define MAX_NO_ADJUST (UINT_MAX-ADJUST_BUFFER)

static m68k_context *sync_components(m68k_context * context, uint32_t address)
{
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	z80_context * z_context = gen->z80;
	//lame estimation of refresh cycle delay
	gen->refresh_counter += context->current_cycle - gen->last_sync_cycle;
	if (!gen->bus_busy) {
		context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	}
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);

	uint32_t mclks = context->current_cycle;
	sync_z80(gen, mclks);
	sync_sound(gen, mclks);
	vdp_run_context(v_context, mclks);
	io_run(gen->io.ports, mclks);
	io_run(gen->io.ports + 1, mclks);
	io_run(gen->io.ports + 2, mclks);
	if (gen->expansion) {
		scd_run(gen->expansion, gen_cycle_to_scd(mclks, gen));
	}
	if (mclks >= gen->reset_cycle) {
		gen->reset_requested = 1;
		context->should_return = 1;
		gen->reset_cycle = CYCLE_NEVER;
	}
	if (v_context->frame != gen->last_frame) {
#ifndef IS_LIB
		if (gen->ym->scope) {
			scope_render(gen->ym->scope);
		}
#endif
		//printf("reached frame end %d | MCLK Cycles: %d, Target: %d, VDP cycles: %d, vcounter: %d, hslot: %d\n", gen->last_frame, mclks, gen->frame_end, v_context->cycles, v_context->vcounter, v_context->hslot);
		uint32_t elapsed = v_context->frame - gen->last_frame;
		gen->last_frame = v_context->frame;
		event_flush(mclks);
		gen->last_flush_cycle = mclks;
		if (gen->header.enter_debugger_frames) {
			if (elapsed >= gen->header.enter_debugger_frames) {
				gen->header.enter_debugger_frames = 0;
				gen->header.enter_debugger = 1;
			} else {
				gen->header.enter_debugger_frames -= elapsed;
			}
		}

		if(exit_after){
			if (elapsed >= exit_after) {
				exit(0);
			} else {
				exit_after -= elapsed;
			}
		}
		if (context->current_cycle > MAX_NO_ADJUST) {
			uint32_t deduction = mclks - ADJUST_BUFFER;
			vdp_adjust_cycles(v_context, deduction);
			io_adjust_cycles(gen->io.ports, context->current_cycle, deduction);
			io_adjust_cycles(gen->io.ports+1, context->current_cycle, deduction);
			io_adjust_cycles(gen->io.ports+2, context->current_cycle, deduction);
			if (gen->mapper_type == MAPPER_JCART) {
				jcart_adjust_cycles(gen, deduction);
			}
			context->current_cycle -= deduction;
			z80_adjust_cycles(z_context, deduction);
			ym_adjust_cycles(gen->ym, deduction);
			if (gen->ym->vgm) {
				vgm_adjust_cycles(gen->ym->vgm, deduction);
			}
			gen->psg->cycles -= deduction;
			if (gen->reset_cycle != CYCLE_NEVER) {
				gen->reset_cycle -= deduction;
			}
			event_cycle_adjust(mclks, deduction);
			if (gen->expansion) {
				scd_adjust_cycle(gen->expansion, deduction);
			}
			gen->last_flush_cycle -= deduction;
		}
	} else if (mclks - gen->last_flush_cycle > gen->soft_flush_cycles) {
		event_soft_flush(mclks);
		gen->last_flush_cycle = mclks;
	}
	gen->frame_end = vdp_cycles_to_frame_end(v_context);
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
	if (context->int_ack) {
		//printf("acknowledging %d @ %d:%d, vcounter: %d, hslot: %d\n", context->int_ack, context->current_cycle, v_context->cycles, v_context->vcounter, v_context->hslot);
		vdp_int_ack(v_context);
		context->int_ack = 0;
	}
	if (!address && (gen->header.enter_debugger || gen->header.save_state)) {
		context->sync_cycle = context->current_cycle + 1;
	}
	adjust_int_cycle(context, v_context);
	if (gen->reset_cycle < context->target_cycle) {
		context->target_cycle = gen->reset_cycle;
	}
	if (address) {
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
#ifndef IS_LIB
			if (gen->header.debugger_type == DEBUGGER_NATIVE) {
				debugger(context, address);
			} else {
				gdb_debug_enter(context, address);
			}
#endif
		}
#ifdef NEW_CORE
		if (gen->header.save_state) {
#else
		if (gen->header.save_state && (z_context->pc || !z_context->native_pc || z_context->reset || !z_context->busreq)) {
#endif
			uint8_t slot = gen->header.save_state - 1;
			gen->header.save_state = 0;
#ifndef NEW_CORE
			if (z_context->native_pc && !z_context->reset) {
				//advance Z80 core to the start of an instruction
				while (!z_context->pc)
				{
					sync_z80(gen, z_context->current_cycle + MCLKS_PER_Z80);
				}
			}
#endif
			char *save_path = slot >= SERIALIZE_SLOT ? NULL : get_slot_name(&gen->header, slot, use_native_states ? "state" : "gst");
			if (use_native_states || slot >= SERIALIZE_SLOT) {
				serialize_buffer state;
				init_serialize(&state);
				genesis_serialize(gen, &state, address, slot != EVENTLOG_SLOT);
				if (slot == SERIALIZE_SLOT) {
					gen->serialize_tmp = state.data;
					gen->serialize_size = state.size;
					context->sync_cycle = context->current_cycle;
					context->should_return = 1;
				} else if (slot == EVENTLOG_SLOT) {
					event_state(context->current_cycle, &state);
				} else {
					save_to_file(&state, save_path);
					free(state.data);
				}
			} else {
				save_gst(gen, save_path, address);
			}
			if (slot != SERIALIZE_SLOT) {
				debug_message("Saved state to %s\n", save_path);
			}
			free(save_path);
		} else if(gen->header.save_state) {
			context->sync_cycle = context->current_cycle + 1;
		}
	}
	gen->last_sync_cycle = context->current_cycle;
	return context;
}

static m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to write to address %X\n", 0xC00000 | vdp_port);
	}
	genesis_context * gen = context->system;
	if (!gen->vdp_unlocked) {
		fatal_error("machine freeze due to VDP write to %X without TMSS unlock\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->current_cycle);

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle;

	sync_components(context, 0);
	vdp_context *v_context = gen->vdp;
	uint32_t before_cycle = v_context->cycles;
	if (vdp_port < 0x10) {
		int blocked;
		if (vdp_port < 4) {
			while (vdp_data_port_write(v_context, value) < 0) {
				while(v_context->flags & FLAG_DMA_RUN) {
					vdp_run_dma_done(v_context, gen->frame_end);
					if (v_context->cycles >= gen->frame_end) {
						uint32_t cycle_diff = v_context->cycles - context->current_cycle;
						uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
						if (m68k_cycle_diff < cycle_diff) {
							m68k_cycle_diff += MCLKS_PER_68K;
						}
						context->current_cycle += m68k_cycle_diff;
						gen->bus_busy = 1;
						sync_components(context, 0);
						gen->bus_busy = 0;
					}
				}
				//context->current_cycle = v_context->cycles;
			}
		} else if(vdp_port < 8) {
			vdp_run_context_full(v_context, context->current_cycle);
			before_cycle = v_context->cycles;
			blocked = vdp_control_port_write(v_context, value, context->current_cycle);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						vdp_run_dma_done(v_context, gen->frame_end);
						if (v_context->cycles >= gen->frame_end) {
							uint32_t cycle_diff = v_context->cycles - context->current_cycle;
							uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
							if (m68k_cycle_diff < cycle_diff) {
								m68k_cycle_diff += MCLKS_PER_68K;
							}
							context->current_cycle += m68k_cycle_diff;
							gen->bus_busy = 1;
							sync_components(context, 0);
							gen->bus_busy = 0;
						}
					}

					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value, context->current_cycle);
					} else {
						blocked = 0;
					}
				}
			} else {
				context->sync_cycle = gen->frame_end = vdp_cycles_to_frame_end(v_context);
				//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
				adjust_int_cycle(context, v_context);
			}
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
		if (v_context->cycles != before_cycle) {
			//printf("68K paused for %d (%d) cycles at cycle %d (%d) for write\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
			uint32_t cycle_diff = v_context->cycles - context->current_cycle;
			uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
			if (m68k_cycle_diff < cycle_diff) {
				m68k_cycle_diff += MCLKS_PER_68K;
			}
			context->current_cycle += m68k_cycle_diff;
			//Lock the Z80 out of the bus until the VDP access is complete
			gen->bus_busy = 1;
			sync_z80(gen, v_context->cycles);
			gen->bus_busy = 0;
		}
	} else if (vdp_port < 0x18) {
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value);
	}

	gen->last_sync_cycle -= 4 * MCLKS_PER_68K;
	//refresh may have happened while we were waiting on the VDP,
	//so advance refresh_counter but don't add any delays
	if (vdp_port >= 4 && vdp_port < 8 && v_context->cycles != before_cycle) {
		gen->refresh_counter = 0;
	} else {
		gen->refresh_counter += (context->current_cycle - gen->last_sync_cycle);
		gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	}
	gen->last_sync_cycle = context->current_cycle;
	return context;
}

static m68k_context * vdp_port_write_b(uint32_t vdp_port, m68k_context * context, uint8_t value)
{
	return vdp_port_write(vdp_port, context, vdp_port < 0x10 ? value | value << 8 : ((vdp_port & 1) ? value : 0));
}

static void * z80_vdp_port_write(uint32_t vdp_port, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	vdp_port &= 0xFF;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to write to Z80 address %X\n", 0x7F00 | vdp_port);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		if (vdp_port < 4) {
			vdp_run_context(gen->vdp, context->Z80_CYCLE);
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_run_context_full(gen->vdp, context->Z80_CYCLE);
			vdp_control_port_write(gen->vdp, value << 8 | value, context->Z80_CYCLE);
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
	} else if (vdp_port < 0x18) {
		sync_sound(gen, context->Z80_CYCLE);
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value);
	}
	return context;
}

static uint16_t vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to read from address %X\n", 0xC00000 | vdp_port);
	}
	genesis_context *gen = context->system;
	if (!gen->vdp_unlocked) {
		fatal_error("machine freeze due to VDP read from %X without TMSS unlock\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	uint16_t value;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle;

	sync_components(context, 0);
	vdp_context * v_context = gen->vdp;
	uint32_t before_cycle = context->current_cycle;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			value = vdp_data_port_read(v_context, &context->current_cycle, MCLKS_PER_68K);
		} else if(vdp_port < 8) {
			value = vdp_control_port_read(v_context);
		} else {
			value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", value, v_context->cycles);
		}
	} else if (vdp_port < 0x18){
		fatal_error("Illegal read from PSG  port %X\n", vdp_port);
	} else {
		value = get_open_bus_value(&gen->header);
	}
	if (context->current_cycle != before_cycle) {
		//printf("68K paused for %d (%d) cycles at cycle %d (%d) for read\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
		//Lock the Z80 out of the bus until the VDP access is complete
		genesis_context *gen = context->system;
		gen->bus_busy = 1;
		sync_z80(gen, context->current_cycle);
		gen->bus_busy = 0;
	}

	gen->last_sync_cycle -= 4 * MCLKS_PER_68K;
	//refresh may have happened while we were waiting on the VDP,
	//so advance refresh_counter but don't add any delays
	gen->refresh_counter += (context->current_cycle - gen->last_sync_cycle);
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle;
	return value;
}

static uint8_t vdp_port_read_b(uint32_t vdp_port, m68k_context * context)
{
	uint16_t value = vdp_port_read(vdp_port, context);
	if (vdp_port & 1) {
		return value;
	} else {
		return value >> 8;
	}
}

static uint8_t z80_vdp_port_read(uint32_t vdp_port, void * vcontext)
{
	z80_context * context = vcontext;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to read from Z80 address %X\n", 0x7F00 | vdp_port);
	}
	genesis_context * gen = context->system;
	//VDP access goes over the 68K bus like a bank area access
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;


	vdp_port &= 0x1F;
	uint16_t ret;
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->Z80_CYCLE);
		uint32_t before = context->Z80_CYCLE;
		if (vdp_port < 4) {
			ret = vdp_data_port_read(gen->vdp, &context->Z80_CYCLE, MCLKS_PER_Z80);
		} else if (vdp_port < 8) {
			ret = vdp_control_port_read(gen->vdp);
		} else {
			ret = vdp_hv_counter_read(gen->vdp);
		}
		if (context->Z80_CYCLE != before) {
			gen->m68k->current_cycle += context->Z80_CYCLE - before;
		}
	} else {
		//TODO: Figure out the correct value today
		ret = 0xFFFF;
	}
	return vdp_port & 1 ? ret : ret >> 8;
}

//TODO: Move this inside the system context
static uint32_t zram_counter = 0;

static m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
	gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle - 4*MCLKS_PER_68K;

	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				gen->zram[location & 0x1FFF] = value;
#ifndef NO_Z80
				z80_handle_code_write(location & 0x1FFF, gen->z80);
#endif
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				if (location & 1) {
					ym_data_write(gen->ym, value);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value);
				} else {
					ym_address_write_part1(gen->ym, value);
				}
			} else if (location == 0x6000) {
				gen->z80_bank_reg = (gen->z80_bank_reg >> 1 | value << 8) & 0x1FF;
				if (gen->z80_bank_reg < 0x80) {
					gen->z80->mem_pointers[1] = (gen->z80_bank_reg << 15) + ((char *)gen->z80->mem_pointers[2]);
				} else {
					gen->z80->mem_pointers[1] = NULL;
				}
			} else {
				fatal_error("68K write to unhandled Z80 address %X\n", location);
			}
		}
	} else {
		if (location < 0x10100) {
			switch(location >> 1 & 0xFF)
			{
			case 0x1:
				io_data_write(gen->io.ports, value, context->current_cycle);
				break;
			case 0x2:
				io_data_write(gen->io.ports+1, value, context->current_cycle);
				break;
			case 0x3:
				io_data_write(gen->io.ports+2, value, context->current_cycle);
				break;
			case 0x4:
				io_control_write(gen->io.ports, value, context->current_cycle);
				break;
			case 0x5:
				io_control_write(gen->io.ports+1, value, context->current_cycle);
				break;
			case 0x6:
				io_control_write(gen->io.ports+2, value, context->current_cycle);
				break;
			case 0x7:
				io_tx_write(gen->io.ports, value, context->current_cycle);
				break;
			case 0x8:
			case 0xB:
			case 0xE:
				//serial input port is not writeable
				break;
			case 0x9:
				io_sctrl_write(gen->io.ports, value, context->current_cycle);
				gen->io.ports[0].serial_ctrl = value;
				break;
			case 0xA:
				io_tx_write(gen->io.ports + 1, value, context->current_cycle);
				break;
			case 0xC:
				io_sctrl_write(gen->io.ports + 1, value, context->current_cycle);
				break;
			case 0xD:
				io_tx_write(gen->io.ports + 2, value, context->current_cycle);
				break;
			case 0xF:
				io_sctrl_write(gen->io.ports + 2, value, context->current_cycle);
				break;
			}
		} else {
			uint32_t masked = location & 0xFFF00;
			if (masked == 0x11100) {
				if (value & 1) {
					dputs("bus requesting Z80");
					if (z80_enabled) {
						z80_assert_busreq(gen->z80, context->current_cycle);
					} else {
						gen->z80->busack = 1;
					}
				} else {
					if (gen->z80->busreq) {
						dputs("releasing z80 bus");
						#ifdef DO_DEBUG_PRINT
						char fname[20];
						sprintf(fname, "zram-%d", zram_counter++);
						FILE * f = fopen(fname, "wb");
						fwrite(z80_ram, 1, sizeof(z80_ram), f);
						fclose(f);
						#endif
					}
					if (z80_enabled) {
						z80_clear_busreq(gen->z80, context->current_cycle);
					} else {
						gen->z80->busack = 0;
					}
				}
			} else if (masked == 0x11200) {
				sync_z80(gen, context->current_cycle);
				if (value & 1) {
					if (z80_enabled) {
						z80_clear_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 0;
					}
				} else {
					if (z80_enabled) {
						z80_assert_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 1;
					}
					ym_reset(gen->ym);
				}
			} else if (masked != 0x11300 && masked != 0x11000) {
				fatal_error("Machine freeze due to unmapped write to address %X\n", location | 0xA00000);
			}
		}
	}

	//no refresh delays during IO access
	gen->refresh_counter += context->current_cycle - gen->last_sync_cycle;
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	return context;
}

static m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	if (location < 0x10000 || (location & 0x1FFF) >= 0x100) {
		return io_write(location, context, value >> 8);
	} else {
		return io_write(location, context, value);
	}
}

#define FOREIGN 0x80
#define HZ50 0x40
#define USA FOREIGN
#define JAP 0x00
#define EUR (HZ50|FOREIGN)
#define NO_DISK 0x20

static uint8_t io_read(uint32_t location, m68k_context * context)
{
	uint8_t value;
	genesis_context *gen = context->system;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
	gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
	context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle - 4*MCLKS_PER_68K;

	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = gen->zram[location & 0x1FFF];
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				value = ym_read_status(gen->ym, context->current_cycle, location);
			} else if (location < 0x7F00) {
				value = 0xFF;
			} else {
				fatal_error("Machine freeze due to read of Z80 VDP memory window by 68K: %X\n", location | 0xA00000);
				value = 0xFF;
			}
		} else {
			uint16_t word = get_open_bus_value(&gen->header);
			value = location & 1 ? word : word >> 8;
		}
	} else {
		if (location < 0x10100) {
			switch(location >> 1 & 0xFF)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				value = gen->version_reg;
				break;
			case 0x1:
				value = io_data_read(gen->io.ports, context->current_cycle);
				break;
			case 0x2:
				value = io_data_read(gen->io.ports+1, context->current_cycle);
				break;
			case 0x3:
				value = io_data_read(gen->io.ports+2, context->current_cycle);
				break;
			case 0x4:
				value = gen->io.ports[0].control;
				break;
			case 0x5:
				value = gen->io.ports[1].control;
				break;
			case 0x6:
				value = gen->io.ports[2].control;
				break;
			case 0x7:
				value = gen->io.ports[0].serial_out;
				break;
			case 0x8:
				value = io_rx_read(gen->io.ports, context->current_cycle);
				break;
			case 0x9:
				value = io_sctrl_read(gen->io.ports, context->current_cycle);
				break;
			case 0xA:
				value = gen->io.ports[1].serial_out;
				break;
			case 0xB:
				value = io_rx_read(gen->io.ports + 1, context->current_cycle);
				break;
			case 0xC:
				value = io_sctrl_read(gen->io.ports, context->current_cycle);
				break;
			case 0xD:
				value = gen->io.ports[2].serial_out;
				break;
			case 0xE:
				value = io_rx_read(gen->io.ports + 1, context->current_cycle);
				break;
			case 0xF:
				value = io_sctrl_read(gen->io.ports, context->current_cycle);
				break;
			default:
				value = get_open_bus_value(&gen->header) >> 8;
			}
		} else {
			uint32_t masked = location & 0xFFF00;
			if (masked == 0x11100) {
				value = z80_enabled ? !z80_get_busack(gen->z80, context->current_cycle) : !gen->z80->busack;
				value |= (get_open_bus_value(&gen->header) >> 8) & 0xFE;
				dprintf("Byte read of BUSREQ returned %d @ %d (reset: %d)\n", value, context->current_cycle, gen->z80->reset);
			} else if (masked == 0x11200) {
				value = !gen->z80->reset;
			} else if (masked == 0x11300 || masked == 0x11000) {
				//A11300 is apparently completely unused
				//A11000 is the memory control register which I am assuming is write only
				value = get_open_bus_value(&gen->header) >> 8;
			} else {
				location |= 0xA00000;
				fatal_error("Machine freeze due to read of unmapped IO location %X\n", location);
				value = 0xFF;
			}
		}
	}

	//no refresh delays during IO access
	gen->refresh_counter += context->current_cycle - gen->last_sync_cycle;
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
	gen->last_sync_cycle = context->current_cycle;
	return value;
}

static uint16_t io_read_w(uint32_t location, m68k_context * context)
{
	genesis_context *gen = context->system;
	uint16_t value = io_read(location, context);
	if (location < 0x10000 || (location & 0x1FFF) < 0x100) {
		value = value | (value << 8);
	} else {
		value <<= 8;
		value |= get_open_bus_value(&gen->header) & 0xFF;
	}
	return value;
}

static void * z80_write_ym(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->Z80_CYCLE);
	if (location & 1) {
		ym_data_write(gen->ym, value);
	} else if (location & 2) {
		ym_address_write_part2(gen->ym, value);
	} else {
		ym_address_write_part1(gen->ym, value);
	}
	return context;
}

static uint8_t z80_read_ym(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->Z80_CYCLE);
	return ym_read_status(gen->ym, context->Z80_CYCLE, location);
}

static uint8_t z80_read_bank(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->Z80_CYCLE = gen->m68k->current_cycle;
	}
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	if (context->mem_pointers[1]) {
		return context->mem_pointers[1][location ^ 1];
	}
	uint32_t address = gen->z80_bank_reg << 15 | location;
	if (address >= 0xC00000 && address < 0xE00000) {
		return z80_vdp_port_read(location & 0xFF, context);
	} else if (address >= 0xA10000 && address <= 0xA10001) {
		//Apparently version reg can be read through Z80 banked area
		//TODO: Check rest of IO region addresses
		return gen->version_reg;
	} else {
		fprintf(stderr, "Unhandled read by Z80 from address %X through banked memory area (%X)\n", address, gen->z80_bank_reg << 15);
	}
	return 0;
}

static void *z80_write_bank(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->Z80_CYCLE = gen->m68k->current_cycle;
	}
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	uint32_t address = gen->z80_bank_reg << 15 | location;
	if (address >= 0xE00000) {
		address &= 0xFFFF;
		((uint8_t *)gen->work_ram)[address ^ 1] = value;
	} else if (address >= 0xC00000) {
		z80_vdp_port_write(location & 0xFF, context, value);
	} else {
		fprintf(stderr, "Unhandled write by Z80 to address %X through banked memory area\n", address);
	}
	return context;
}

static void *z80_write_bank_reg(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;

	gen->z80_bank_reg = (gen->z80_bank_reg >> 1 | value << 8) & 0x1FF;
	update_z80_bank_pointer(context->system);

	return context;
}

static uint16_t unused_read(uint32_t location, void *vcontext)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	if (location >= 0x800000) {
		//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
		gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
		context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
		gen->refresh_counter += 4*MCLKS_PER_68K;
		gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
		gen->last_sync_cycle = context->current_cycle;
	}

	if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//Only called if the cart/exp doesn't have a more specific handler for this region
		return get_open_bus_value(&gen->header);
	} else if (location == 0xA14000 || location == 0xA14002) {
		if (gen->version_reg & 0xF) {
			return gen->tmss_lock[location >> 1 & 1];
		} else {
			fatal_error("Machine freeze due to read from TMSS lock when TMSS is not present %X\n", location);
			return 0xFFFF;
		}
	} else if (location == 0xA14100) {
		if (gen->version_reg & 0xF) {
			return get_open_bus_value(&gen->header);
		} else {
			fatal_error("Machine freeze due to read from TMSS control when TMSS is not present %X\n", location);
			return 0xFFFF;
		}
	} else {
		fatal_error("Machine freeze due to unmapped read from %X\n", location);
		return 0xFFFF;
	}
}

static uint8_t unused_read_b(uint32_t location, void *vcontext)
{
	uint16_t v = unused_read(location & 0xFFFFFE, vcontext);
	if (location & 1) {
		return v;
	} else {
		return v >> 8;
	}
}

static void check_tmss_lock(genesis_context *gen)
{
	gen->vdp_unlocked = gen->tmss_lock[0] == 0x5345 && gen->tmss_lock[1] == 0x4741;
}

static void toggle_tmss_rom(genesis_context *gen)
{
	m68k_context *context = gen->m68k;
	for (int i = 0; i < NUM_MEM_AREAS; i++)
	{
		uint16_t *tmp = context->mem_pointers[i];
		context->mem_pointers[i] = gen->tmss_pointers[i];
		gen->tmss_pointers[i] = tmp;
	}
	m68k_invalidate_code_range(context, 0, 0x400000);
}

static void *unused_write(uint32_t location, void *vcontext, uint16_t value)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	if (location >= 0x800000) {
		//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
		gen->refresh_counter += context->current_cycle - 4*MCLKS_PER_68K - gen->last_sync_cycle;
		context->current_cycle += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
		gen->refresh_counter += 4*MCLKS_PER_68K;
		gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
		gen->last_sync_cycle = context->current_cycle;
	}

	uint8_t has_tmss = gen->version_reg & 0xF;
	if (has_tmss && (location == 0xA14000 || location == 0xA14002)) {
		gen->tmss_lock[location >> 1 & 1] = value;
		check_tmss_lock(gen);
	} else if (has_tmss && location == 0xA14100) {
		value &= 1;
		if (gen->tmss != value) {
			gen->tmss = value;
			toggle_tmss_rom(gen);
		}
	} else if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//these writes are ignored when no relevant hardware is present
	} else {
		fatal_error("Machine freeze due to unmapped write to %X\n", location);
	}
	return vcontext;
}

static void *unused_write_b(uint32_t location, void *vcontext, uint8_t value)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	uint8_t has_tmss = gen->version_reg & 0xF;
	if (has_tmss && location >= 0xA14000 && location <= 0xA14003) {
		uint32_t offset = location >> 1 & 1;
		if (location & 1) {
			gen->tmss_lock[offset] &= 0xFF00;
			gen->tmss_lock[offset] |= value;
		} else {
			gen->tmss_lock[offset] &= 0xFF;
			gen->tmss_lock[offset] |= value << 8;
		}
		check_tmss_lock(gen);
	} else if (has_tmss && (location == 0xA14100 || location == 0xA14101)) {
		if (location & 1) {
			value &= 1;
			if (gen->tmss != value) {
				gen->tmss = value;
				toggle_tmss_rom(gen);
			}
		}
	} else if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//these writes are ignored when no relevant hardware is present
	} else {
		fatal_error("Machine freeze due to unmapped byte write to %X\n", location);
	}
	return vcontext;
}

static void set_speed_percent(system_header * system, uint32_t percent)
{
	genesis_context *context = (genesis_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;
	while (context->ym->current_cycle != context->psg->cycles) {
		sync_sound(context, context->psg->cycles + MCLKS_PER_PSG);
	}
	if (context->expansion) {
		segacd_context *cd = context->expansion;
		segacd_set_speed_percent(cd, percent);
	}
	ym_adjust_master_clock(context->ym, context->master_clock);
	psg_adjust_master_clock(context->psg, context->master_clock);
}

void set_region(genesis_context *gen, rom_info *info, uint8_t region)
{
	if (!region) {
		char * def_region = tern_find_path_default(config, "system\0default_region\0", (tern_val){.ptrval = "U"}, TVAL_PTR).ptrval;
		if (!info->regions || (info->regions & translate_region_char(toupper(*def_region)))) {
			region = translate_region_char(toupper(*def_region));
		} else {
			region = info->regions;
		}
	}
	if (region & REGION_E) {
		gen->version_reg = NO_DISK | EUR;
	} else if (region & REGION_J) {
		gen->version_reg = NO_DISK | JAP;
	} else {
		gen->version_reg = NO_DISK | USA;
	}

	if (region & HZ50) {
		gen->normal_clock = MCLKS_PAL;
		gen->soft_flush_cycles = MCLKS_LINE * 262 / 3 + 2;
	} else {
		gen->normal_clock = MCLKS_NTSC;
		gen->soft_flush_cycles = MCLKS_LINE * 313 / 3 + 2;
	}
	gen->master_clock = gen->normal_clock;
}

static uint8_t load_state(system_header *system, uint8_t slot)
{
	genesis_context *gen = (genesis_context *)system;
	char *statepath = get_slot_name(system, slot, "state");
	deserialize_buffer state;
	uint32_t pc = 0;
	uint8_t ret;
	if (!gen->m68k->resume_pc) {
		system->delayed_load_slot = slot + 1;
		gen->m68k->should_return = 1;
		ret = get_modification_time(statepath) != 0;
		if (!ret) {
			strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
			ret = get_modification_time(statepath) != 0;
		}
		goto done;
	}
	if (load_from_file(&state, statepath)) {
		genesis_deserialize(&state, gen);
		free(state.data);
		ret = 1;
	} else {
		strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
		pc = load_gst(gen, statepath);
		ret = pc != 0;
	}
	if (ret) {
		debug_message("Loaded state from %s\n", statepath);
	}
done:
	free(statepath);
	return ret;
}

static void handle_reset_requests(genesis_context *gen)
{
	while (gen->reset_requested || gen->header.delayed_load_slot)
	{
		if (gen->reset_requested) {
			gen->reset_requested = 0;
			gen->m68k->should_return = 0;
			z80_assert_reset(gen->z80, gen->m68k->current_cycle);
			z80_clear_busreq(gen->z80, gen->m68k->current_cycle);
			ym_reset(gen->ym);
			//Is there any sort of VDP reset?
			m68k_reset(gen->m68k);
		}
		if (gen->header.delayed_load_slot) {
			load_state(&gen->header, gen->header.delayed_load_slot - 1);
			gen->header.delayed_load_slot = 0;
			resume_68k(gen->m68k);
		}
	}
	if (gen->header.force_release || render_should_release_on_exit()) {
		bindings_release_capture();
		vdp_release_framebuffer(gen->vdp);
		render_pause_source(gen->ym->audio);
		render_pause_source(gen->psg->audio);
	}
}

static void start_genesis(system_header *system, char *statefile)
{
	genesis_context *gen = (genesis_context *)system;
	if (statefile) {
		//first try loading as a native format savestate
		deserialize_buffer state;
		uint32_t pc;
		if (load_from_file(&state, statefile)) {
			genesis_deserialize(&state, gen);
			free(state.data);
			//HACK
			pc = gen->m68k->last_prefetch_address;
		} else {
			pc = load_gst(gen, statefile);
			if (!pc) {
				fatal_error("Failed to load save state %s\n", statefile);
			}
		}
		printf("Loaded %s\n", statefile);
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
#ifndef IS_LIB
			insert_breakpoint(gen->m68k, pc, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
#endif
		}
		adjust_int_cycle(gen->m68k, gen->vdp);
		start_68k_context(gen->m68k, pc);
	} else {
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
#ifndef IS_LIB
			uint32_t address = read_word(4, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen, gen->m68k) << 16
				| read_word(6, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen, gen->m68k);
			insert_breakpoint(gen->m68k, address, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
#endif
		}
		m68k_reset(gen->m68k);
	}
	handle_reset_requests(gen);
	return;
}

static void resume_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->header.force_release || render_should_release_on_exit()) {
		gen->header.force_release = 0;
		render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
		bindings_reacquire_capture();
		vdp_reacquire_framebuffer(gen->vdp);
		render_resume_source(gen->ym->audio);
		render_resume_source(gen->psg->audio);
	}
	resume_68k(gen->m68k);
	handle_reset_requests(gen);
}

static void inc_debug_mode(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	vdp_inc_debug_mode(gen->vdp);
}

static void request_exit(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->m68k->target_cycle = gen->m68k->current_cycle;
	gen->m68k->should_return = 1;
}

static void persist_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	FILE *f;
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		char *bram_name = path_append(system->save_dir, "internal.bram");
		f = fopen(bram_name, "wb");
		if (f) {
			fwrite(cd->bram, 1, 8 * 1024, f);
			fclose(f);
			printf("Saved internal BRAM to %s\n", bram_name);
		}
		free(bram_name);
		if (cd->bram_cart_id < 8) {
			bram_name = path_append(system->save_dir, "cart.bram");
			f = fopen(bram_name, "wb");
			if (f) {
				long configured_size = 0x2000 << cd->bram_cart_id;
				fwrite(cd->bram_cart, 1, configured_size, f);
				fclose(f);
				printf("Saved BRAM cart to %s\n", bram_name);
			}
			free(bram_name);
		}
	}
	if (gen->save_type == SAVE_NONE) {
		return;
	}
	f = fopen(save_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open %s file %s for writing\n", save_type_name(gen->save_type), save_filename);
		return;
	}
	if (gen->save_type == RAM_FLAG_BOTH) {
		byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
	}
	fwrite(gen->save_storage, 1, gen->save_size, f);
	if (gen->save_type == RAM_FLAG_BOTH) {
		byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
	}
	fclose(f);
	printf("Saved %s to %s\n", save_type_name(gen->save_type), save_filename);
}

static void load_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	FILE * f = fopen(save_filename, "rb");
	if (f) {
		uint32_t read = fread(gen->save_storage, 1, gen->save_size, f);
		fclose(f);
		if (read > 0) {
			if (gen->save_type == RAM_FLAG_BOTH) {
				byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
			}
			printf("Loaded %s from %s\n", save_type_name(gen->save_type), save_filename);
		}
	}
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		char *bram_name = path_append(system->save_dir, "internal.bram");
		f = fopen(bram_name, "rb");
		if (f) {
			uint32_t read = fread(cd->bram, 1, 8 * 1024, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded internal BRAM from %s\n", bram_name);
			}
		}
		free(bram_name);
		bram_name = path_append(system->save_dir, "cart.bram");
		f = fopen(bram_name, "rb");
		if (f) {
			long existing_size = nearest_pow2(file_size(f));
			if (existing_size > 1 * 1024 * 1024) {
				existing_size = 1 * 1024 * 1024;
			}
			long configured_size = 0x2000 << cd->bram_cart_id;
			if (existing_size != configured_size) {
				if (existing_size > configured_size) {
					free(cd->bram_cart);
					cd->bram_cart = calloc(existing_size, 1);
				}
				cd->bram_cart_id = 0;
				while (existing_size > (0x2000 <<cd->bram_cart_id)) {
					cd->bram_cart_id++;
				}
			}
			uint32_t read = fread(cd->bram_cart, 1, existing_size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded BRAM cart from %s\n", bram_name);
			}
		}
		free(bram_name);
	}
}

static void soft_reset(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->reset_cycle == CYCLE_NEVER) {
		double random = (double)rand()/(double)RAND_MAX;
		gen->reset_cycle = gen->m68k->current_cycle + random * MCLKS_LINE * (gen->version_reg & HZ50 ? LINES_PAL : LINES_NTSC);
		if (gen->reset_cycle < gen->m68k->target_cycle) {
			gen->m68k->target_cycle = gen->reset_cycle;
		}
	}
}

static void free_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->expansion) {
		free_segacd(gen->expansion);
	}
	vdp_free(gen->vdp);
	memmap_chunk *map = (memmap_chunk *)gen->m68k->options->gen.memmap;
	m68k_options_free(gen->m68k->options);
	free(gen->cart);
	free(gen->m68k);
	free(gen->work_ram);
	z80_options_free(gen->z80->Z80_OPTS);
	free(gen->z80);
	free(gen->zram);
	ym_free(gen->ym);
	psg_free(gen->psg);
	free(gen->header.save_dir);
	free_rom_info(&gen->header.info);
	free(gen->lock_on);
	if (gen->save_type != SAVE_NONE && gen->mapper_type != MAPPER_SEGA_MED_V2) {
		free(gen->save_storage);
	}
	free(map);
	free(gen);
}

static void gamepad_down(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_gamepad_down(&gen->io, gamepad_num, button);
	if (gen->mapper_type == MAPPER_JCART) {
		jcart_gamepad_down(gen, gamepad_num, button);
	}
}

static void gamepad_up(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_gamepad_up(&gen->io, gamepad_num, button);
	if (gen->mapper_type == MAPPER_JCART) {
		jcart_gamepad_up(gen, gamepad_num, button);
	}
}

static void mouse_down(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_down(&gen->io, mouse_num, button);
}

static void mouse_up(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_up(&gen->io, mouse_num, button);
}

static void mouse_motion_absolute(system_header *system, uint8_t mouse_num, uint16_t x, uint16_t y)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_motion_absolute(&gen->io, mouse_num, x, y);
}

static void mouse_motion_relative(system_header *system, uint8_t mouse_num, int32_t x, int32_t y)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_motion_relative(&gen->io, mouse_num, x, y);
}

static void keyboard_down(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	io_keyboard_down(&gen->io, scancode);
}

static void keyboard_up(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	io_keyboard_up(&gen->io, scancode);
}

static void set_audio_config(genesis_context *gen)
{
	char *config_gain;
	config_gain = tern_find_path(config, "audio\0psg_gain\0", TVAL_PTR).ptrval;
	render_audio_source_gaindb(gen->psg->audio, config_gain ? atof(config_gain) : 0.0f);
	config_gain = tern_find_path(config, "audio\0fm_gain\0", TVAL_PTR).ptrval;
	render_audio_source_gaindb(gen->ym->audio, config_gain ? atof(config_gain) : 0.0f);

	char *config_dac = tern_find_path_default(config, "audio\0fm_dac\0", (tern_val){.ptrval="zero_offset"}, TVAL_PTR).ptrval;
	ym_enable_zero_offset(gen->ym, !strcmp(config_dac, "zero_offset"));

	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		config_gain = tern_find_path(config, "audio\0rf5c164_gain\0", TVAL_PTR).ptrval;
		render_audio_source_gaindb(cd->pcm.audio, config_gain ? atof(config_gain) : -6.0f);
		config_gain = tern_find_path(config, "audio\0cdda_gain\0", TVAL_PTR).ptrval;
		render_audio_source_gaindb(cd->fader.audio, config_gain ? atof(config_gain) : -9.5f);
	}
}

static void config_updated(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	setup_io_devices(config, &system->info, &gen->io);
	set_audio_config(gen);
}

static void start_vgm_log(system_header *system, char *filename)
{
	genesis_context *gen = (genesis_context *)system;
	vgm_writer *vgm = vgm_write_open(filename, gen->version_reg & HZ50 ? 50 : 60, gen->normal_clock, gen->m68k->current_cycle);
	if (vgm) {
		printf("Started logging VGM to %s\n", filename);
		sync_sound(gen, vgm->last_cycle);
		ym_vgm_log(gen->ym, gen->normal_clock, vgm);
		psg_vgm_log(gen->psg, gen->normal_clock, vgm);
		gen->header.vgm_logging = 1;
	} else {
		printf("Failed to start logging to %s\n", filename);
	}
}

static void stop_vgm_log(system_header *system)
{
	puts("Stopped VGM log");
	genesis_context *gen = (genesis_context *)system;
	vgm_close(gen->ym->vgm);
	gen->ym->vgm = gen->psg->vgm = NULL;
	gen->header.vgm_logging = 0;
}

static void toggle_debug_view(system_header *system, uint8_t debug_view)
{
#ifndef IS_LIB
	genesis_context *gen = (genesis_context *)system;
	if (debug_view < DEBUG_OSCILLOSCOPE) {
		vdp_toggle_debug_view(gen->vdp, debug_view);
	} else if (debug_view == DEBUG_OSCILLOSCOPE) {
		if (gen->ym->scope) {
			oscilloscope *scope = gen->ym->scope;
			gen->ym->scope = NULL;
			gen->psg->scope = NULL;
			if (gen->expansion) {
				segacd_context *cd = gen->expansion;
				cd->pcm.scope = NULL;
			}
			scope_close(scope);
		} else {
			oscilloscope *scope = create_oscilloscope();
			ym_enable_scope(gen->ym, scope, gen->normal_clock);
			psg_enable_scope(gen->psg, scope, gen->normal_clock);
			if (gen->expansion) {
				segacd_context *cd = gen->expansion;
				rf5c164_enable_scope(&cd->pcm, scope);
			}
		}
	} else if (debug_view == DEBUG_CD_GRAPHICS && gen->expansion) {
		scd_toggle_graphics_debug(gen->expansion);
	}
#endif
}

static void *tmss_rom_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_write_16(address, context, value);
	}

	return context;
}

static void *tmss_rom_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_write_8(address, context, value);
	}

	return context;
}

static uint16_t tmss_rom_read_16(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_read_16(address, context);
	}
	return ((uint16_t *)gen->tmss_buffer)[address >> 1];
}

static uint8_t tmss_rom_read_8(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_read_8(address, context);
	}
#ifdef BLASTEM_BIG_ENDIAN
	return gen->tmss_buffer[address];
#else
	return gen->tmss_buffer[address ^ 1];
#endif
}

static void *tmss_word_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		address += gen->tmss_write_offset;
		uint16_t *dest = get_native_pointer(address, (void **)m68k->mem_pointers, &m68k->options->gen);
		*dest = value;
		m68k_handle_code_write(address, m68k);
	}

	return context;
}

static void *tmss_word_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		address += gen->tmss_write_offset;
		uint8_t *dest = get_native_pointer(address & ~1, (void **)m68k->mem_pointers, &m68k->options->gen);
#ifdef BLASTEM_BIG_ENDIAN
		dest[address & 1] = value;
#else
		dest[address & 1 ^ 1] = value;
#endif
		m68k_handle_code_write(address & ~1, m68k);
	}

	return context;
}

static void *tmss_odd_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->options->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static void *tmss_odd_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss && (address & 1)) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->options->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static void *tmss_even_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->options->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value >> 8;
	}
	return context;
}

static void *tmss_even_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss && !(address & 1)) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->options->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static genesis_context *shared_init(uint32_t system_opts, rom_info *rom, uint8_t force_region)
{
	static memmap_chunk z80_map[] = {
		{ 0x0000, 0x4000,  0x1FFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
		{ 0x8000, 0x10000, 0x7FFF, .read_8 = z80_read_bank, .write_8 = z80_write_bank},
		{ 0x4000, 0x6000,  0x0003, .read_8 = z80_read_ym, .write_8 = z80_write_ym},
		{ 0x6000, 0x6100,  0xFFFF, .write_8 = z80_write_bank_reg},
		{ 0x7F00, 0x8000,  0x00FF, .read_8 = z80_vdp_port_read, .write_8 = z80_vdp_port_write}
	};

	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}

	genesis_context *gen = calloc(1, sizeof(genesis_context));
	gen->header.set_speed_percent = set_speed_percent;
	gen->header.start_context = start_genesis;
	gen->header.resume_context = resume_genesis;
	gen->header.load_save = load_save;
	gen->header.persist_save = persist_save;
	gen->header.load_state = load_state;
	gen->header.soft_reset = soft_reset;
	gen->header.free_context = free_genesis;
	gen->header.get_open_bus_value = get_open_bus_value;
	gen->header.request_exit = request_exit;
	gen->header.inc_debug_mode = inc_debug_mode;
	gen->header.gamepad_down = gamepad_down;
	gen->header.gamepad_up = gamepad_up;
	gen->header.mouse_down = mouse_down;
	gen->header.mouse_up = mouse_up;
	gen->header.mouse_motion_absolute = mouse_motion_absolute;
	gen->header.mouse_motion_relative = mouse_motion_relative;
	gen->header.keyboard_down = keyboard_down;
	gen->header.keyboard_up = keyboard_up;
	gen->header.config_updated = config_updated;
	gen->header.serialize = serialize;
	gen->header.deserialize = deserialize;
	gen->header.start_vgm_log = start_vgm_log;
	gen->header.stop_vgm_log = stop_vgm_log;
	gen->header.toggle_debug_view = toggle_debug_view;
	gen->header.type = SYSTEM_GENESIS;
	gen->header.info = *rom;
	set_region(gen, rom, force_region);
	tern_node *model = get_model(config, SYSTEM_GENESIS);
	uint8_t tmss = !strcmp(tern_find_ptr_default(model, "tmss", "off"), "on");
	if (tmss) {
		gen->version_reg |= 1;
	} else {
		gen->vdp_unlocked = 1;
	}

	uint8_t max_vsram = !strcmp(tern_find_ptr_default(model, "vsram", "40"), "64");
	gen->vdp = init_vdp_context(gen->version_reg & 0x40, max_vsram, VDP_GENESIS);
	gen->vdp->system = &gen->header;
	gen->frame_end = vdp_cycles_to_frame_end(gen->vdp);
	char * config_cycles = tern_find_path(config, "clocks\0max_cycles\0", TVAL_PTR).ptrval;
	gen->max_cycles = config_cycles ? atoi(config_cycles) : DEFAULT_SYNC_INTERVAL;
	gen->int_latency_prev1 = MCLKS_PER_68K * 32;
	gen->int_latency_prev2 = MCLKS_PER_68K * 16;

	render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
	event_system_start(SYSTEM_GENESIS, (gen->version_reg & HZ50) ? VID_PAL : VID_NTSC, rom->name);

	gen->ym = malloc(sizeof(ym2612_context));
	char *fm = tern_find_ptr_default(model, "fm", "discrete 2612");
	if (!strcmp(fm + strlen(fm) -4, "3834")) {
		system_opts |= YM_OPT_3834;
	}
	ym_init(gen->ym, gen->master_clock, MCLKS_PER_YM, system_opts);

	gen->psg = malloc(sizeof(psg_context));
	psg_init(gen->psg, gen->master_clock, MCLKS_PER_PSG);

	z80_map[0].buffer = gen->zram = calloc(1, Z80_RAM_BYTES);
#ifndef NO_Z80
	z80_options *z_opts = malloc(sizeof(z80_options));
	init_z80_opts(z_opts, z80_map, 5, NULL, 0, MCLKS_PER_Z80, 0xFFFF);
	gen->z80 = init_z80_context(z_opts);
#ifndef NEW_CORE
	gen->z80->next_int_pulse = z80_next_int_pulse;
#endif
	z80_assert_reset(gen->z80, 0);
#else
	gen->z80 = calloc(1, sizeof(z80_context));
#endif

	gen->z80->system = gen;
	gen->z80->mem_pointers[0] = gen->zram;
	gen->z80->mem_pointers[1] = gen->z80->mem_pointers[2] = NULL;

	gen->work_ram = calloc(2, RAM_WORDS);
	if (!strcmp("random", tern_find_path_default(config, "system\0ram_init\0", (tern_val){.ptrval = "zero"}, TVAL_PTR).ptrval))
	{
		srand(time(NULL));
		for (int i = 0; i < RAM_WORDS; i++)
		{
			gen->work_ram[i] = rand();
		}
		for (int i = 0; i < Z80_RAM_BYTES; i++)
		{
			gen->zram[i] = rand();
		}
		for (int i = 0; i < VRAM_SIZE; i++)
		{
			gen->vdp->vdpmem[i] = rand();
		}
		for (int i = 0; i < SAT_CACHE_SIZE; i++)
		{
			gen->vdp->sat_cache[i] = rand();
		}
		for (int i = 0; i < CRAM_SIZE; i++)
		{
			write_cram_internal(gen->vdp, i, rand());
		}
		for (int i = 0; i < gen->vdp->vsram_size; i++)
		{
			gen->vdp->vsram[i] = rand();
		}
	}

	return gen;
}

static memmap_chunk base_map[] = {
	{0xE00000, 0x1000000, 0xFFFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
	{0xC00000, 0xE00000,  0x1FFFFF, .read_16 = (read_16_fun)vdp_port_read,  .write_16 =(write_16_fun)vdp_port_write,
			   .read_8 = (read_8_fun)vdp_port_read_b, .write_8 = (write_8_fun)vdp_port_write_b},
	{0xA00000, 0xA12000,  0x1FFFF,  .read_16 = (read_16_fun)io_read_w, .write_16 = (write_16_fun)io_write_w,
			   .read_8 = (read_8_fun)io_read, .write_8 = (write_8_fun)io_write},
	{0x000000, 0xFFFFFF, 0xFFFFFF, .read_16 = (read_16_fun)unused_read, .write_16 = unused_write,
			   .read_8 = (read_8_fun)unused_read_b, .write_8 = (write_8_fun)unused_write_b}
};
const size_t base_chunks = sizeof(base_map)/sizeof(*base_map);

genesis_context *alloc_config_genesis(void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t ym_opts, uint8_t force_region)
{
	tern_node *rom_db = get_rom_db();
	rom_info info = configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, base_map, base_chunks);
	rom = info.rom;
	rom_size = info.rom_size;
#ifndef BLASTEM_BIG_ENDIAN
	byteswap_rom(nearest_pow2(rom_size), rom);
	if (lock_on) {
		byteswap_rom(nearest_pow2(lock_on_size), lock_on);
	}
#endif
	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}
	genesis_context *gen = shared_init(ym_opts, &info, force_region);
	gen->z80->mem_pointers[1] = gen->z80->mem_pointers[2] = rom;

	gen->cart = rom;
	gen->lock_on = lock_on;

	setup_io_devices(config, &info, &gen->io);
	gen->header.has_keyboard = io_has_keyboard(&gen->io);
	gen->mapper_type = info.mapper_type;
	gen->save_type = info.save_type;
	if (gen->save_type != SAVE_NONE) {
		gen->save_ram_mask = info.save_mask;
		gen->save_size = info.save_size;
		gen->save_storage = info.save_buffer;
		gen->header.info.save_buffer = info.save_buffer = NULL;
		gen->eeprom_map = info.eeprom_map;
		gen->num_eeprom = info.num_eeprom;
		if (gen->save_type == SAVE_I2C) {
			eeprom_init(&gen->eeprom, gen->save_storage, gen->save_size);
		} else if (gen->save_type == SAVE_NOR) {
			memcpy(&gen->nor, info.nor, sizeof(gen->nor));
			//nor_flash_init(&gen->nor, gen->save_storage, gen->save_size, info.save_page_size, info.save_product_id, info.save_bus);
		}
	} else {
		gen->save_storage = NULL;
	}

	gen->mapper_start_index = info.mapper_start_index;

	tern_node *model = get_model(config, SYSTEM_GENESIS);
	uint8_t tmss = !strcmp(tern_find_ptr_default(model, "tmss", "off"), "on");

	//This must happen before we generate memory access functions in init_m68k_opts
	uint8_t next_ptr_index = 0;
	uint32_t tmss_min_alloc = 16 * 1024;
	for (int i = 0; i < info.map_chunks; i++)
	{
		if (info.map[i].start == 0xE00000) {
			info.map[i].buffer = gen->work_ram;
			if (!tmss) {
				break;
			}
		}
		if (info.map[i].flags & MMAP_PTR_IDX && info.map[i].ptr_index >= next_ptr_index) {
			next_ptr_index = info.map[i].ptr_index + 1;
		}
		if (info.map[i].start < 0x400000 && info.map[i].read_16 != unused_read) {
			uint32_t highest_offset = (info.map[i].end & info.map[i].mask) + 1;
			if (highest_offset > tmss_min_alloc) {
				tmss_min_alloc = highest_offset;
			}
		}
	}
	if (tmss) {
		char *tmss_path = tern_find_path_default(config, "system\0tmss_path\0", (tern_val){.ptrval = "tmss.md"}, TVAL_PTR).ptrval;
		uint8_t *buffer = malloc(tmss_min_alloc);
		uint32_t tmss_size;
		if (is_absolute_path(tmss_path)) {
			FILE *f = fopen(tmss_path, "rb");
			if (!f) {
				fatal_error("Configured to use a model with TMSS, but failed to load the TMSS ROM from %s\n", tmss_path);
			}
			tmss_size = fread(buffer, 1, tmss_min_alloc, f);
			fclose(f);
		} else {
			char *tmp = read_bundled_file(tmss_path, &tmss_size);
			if (!tmp) {
				fatal_error("Configured to use a model with TMSS, but failed to load the TMSS ROM from %s\n", tmss_path);
			}
			memcpy(buffer, tmp, tmss_size);
			free(tmp);
		}
		for (uint32_t padded = nearest_pow2(tmss_size); tmss_size < padded; tmss_size++)
		{
			buffer[tmss_size] = 0xFF;
		}
#ifndef BLASTEM_BIG_ENDIAN
		byteswap_rom(tmss_size, (uint16_t *)buffer);
#endif
		//mirror TMSS ROM until we fill up to tmss_min_alloc
		for (uint32_t dst = tmss_size; dst < tmss_min_alloc; dst += tmss_size)
		{
			memcpy(buffer + dst, buffer, dst + tmss_size > tmss_min_alloc ? tmss_min_alloc - dst : tmss_size);
		}
		//modify mappings for ROM space to point to the TMSS ROM and fixup flags to allow switching back and forth
		//WARNING: This code makes some pretty big assumptions about the kinds of map chunks it will encounter
		for (int i = 0; i < info.map_chunks; i++)
		{
			if (info.map[i].start < 0x400000 && info.map[i].read_16 != unused_read) {
				if (info.map[i].flags == MMAP_READ) {
					//Normal ROM
					info.map[i].flags |= MMAP_PTR_IDX | MMAP_CODE;
					info.map[i].ptr_index = next_ptr_index++;
					if (info.map[i].ptr_index >= NUM_MEM_AREAS) {
						fatal_error("Too many memmap chunks with MMAP_PTR_IDX after TMSS remap\n");
					}
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
				} else if (info.map[i].flags & MMAP_PTR_IDX) {
					//Sega mapper page or multi-game mapper
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (info.map[i].write_16) {
						if (!gen->tmss_write_16) {
							gen->tmss_write_16 = info.map[i].write_16;
							gen->tmss_write_8 = info.map[i].write_8;
							info.map[i].write_16 = tmss_rom_write_16;
							info.map[i].write_8 = tmss_rom_write_8;
						} else if (gen->tmss_write_16 == info.map[i].write_16) {
							info.map[i].write_16 = tmss_rom_write_16;
							info.map[i].write_8 = tmss_rom_write_8;
						} else {
							warning("Chunk starting at %X has a write function, but we've already stored a different one for TMSS remap\n", info.map[i].start);
						}
					}
				} else if ((info.map[i].flags & (MMAP_READ | MMAP_WRITE)) == (MMAP_READ | MMAP_WRITE)) {
					//RAM or SRAM
					info.map[i].flags |= MMAP_PTR_IDX;
					info.map[i].ptr_index = next_ptr_index++;
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (!gen->tmss_write_offset || gen->tmss_write_offset == info.map[i].start) {
						gen->tmss_write_offset = info.map[i].start;
						info.map[i].flags &= ~MMAP_WRITE;
						if (info.map[i].flags & MMAP_ONLY_ODD) {
							info.map[i].write_16 = tmss_odd_write_16;
							info.map[i].write_8 = tmss_odd_write_8;
						} else if (info.map[i].flags & MMAP_ONLY_EVEN) {
							info.map[i].write_16 = tmss_even_write_16;
							info.map[i].write_8 = tmss_even_write_8;
						} else {
							info.map[i].write_16 = tmss_word_write_16;
							info.map[i].write_8 = tmss_word_write_8;
						}
					} else {
						warning("Could not remap writes for chunk starting at %X for TMSS because write_offset is %X\n", info.map[i].start, gen->tmss_write_offset);
					}
				} else if (info.map[i].flags & MMAP_READ_CODE) {
					//NOR flash
					info.map[i].flags |= MMAP_PTR_IDX;
					info.map[i].ptr_index = next_ptr_index++;
					if (info.map[i].ptr_index >= NUM_MEM_AREAS) {
						fatal_error("Too many memmap chunks with MMAP_PTR_IDX after TMSS remap\n");
					}
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (!gen->tmss_write_16) {
						gen->tmss_write_16 = info.map[i].write_16;
						gen->tmss_write_8 = info.map[i].write_8;
						gen->tmss_read_16 = info.map[i].read_16;
						gen->tmss_read_8 = info.map[i].read_8;
						info.map[i].write_16 = tmss_rom_write_16;
						info.map[i].write_8 = tmss_rom_write_8;
						info.map[i].read_16 = tmss_rom_read_16;
						info.map[i].read_8 = tmss_rom_read_8;
					} else if (gen->tmss_write_16 == info.map[i].write_16) {
						info.map[i].write_16 = tmss_rom_write_16;
						info.map[i].write_8 = tmss_rom_write_8;
						info.map[i].read_16 = tmss_rom_read_16;
						info.map[i].read_8 = tmss_rom_read_8;
					} else {
						warning("Chunk starting at %X has a write function, but we've already stored a different one for TMSS remap\n", info.map[i].start);
					}
				} else {
					warning("Didn't remap chunk starting at %X for TMSS because it has flags %X\n", info.map[i].start, info.map[i].flags);
				}
			}
		}
		gen->tmss_buffer = buffer;
	}
	memmap_chunk* map = info.map;
	uint32_t map_chunks = info.map_chunks;
	if (info.wants_cd || (current_media()->chain && current_media()->chain->type == MEDIA_CDROM)) {
		gen->header.type = SYSTEM_SEGACD;
		segacd_context *cd = alloc_configure_segacd((system_media *)current_media(), 0, force_region, &info);
		gen->expansion = cd;
		gen->version_reg &= ~NO_DISK;
		cd->genesis = gen;
		uint32_t cd_chunks;
		memmap_chunk *cd_map = segacd_main_cpu_map(gen->expansion, 1, &cd_chunks);
		map_chunks = cd_chunks + info.map_chunks;
		map = calloc(map_chunks, sizeof(memmap_chunk));
		memcpy(map, info.map, sizeof(memmap_chunk) * (info.map_chunks - 1));
		memcpy(map + info.map_chunks - 1, cd_map, sizeof(memmap_chunk) * cd_chunks);
		memcpy(map + map_chunks - 1, info.map + info.map_chunks - 1, sizeof(memmap_chunk));
		free(info.map);
		int max_ptr_index = -1;
		for (int i = 0; i < info.map_chunks - 1; i++)
		{
			if (map[i].flags & MMAP_PTR_IDX && map[i].ptr_index > max_ptr_index) {
				max_ptr_index = map[i].ptr_index;
			}
		}
		cd->memptr_start_index = max_ptr_index + 1;
		for (int i = info.map_chunks - 1; i < map_chunks - 1; i++)
		{
			if (map[i].flags & MMAP_PTR_IDX) {
				map[i].ptr_index += cd->memptr_start_index;
			}
		}
		cd->base = 0x400000;
		cd->m68k->options->address_log = (ym_opts & OPT_ADDRESS_LOG) ? fopen("address_sub.log", "w") : NULL;
	}
	info.map = gen->header.info.map = NULL;

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, map_chunks, MCLKS_PER_68K, sync_components);
	if (!strcmp(tern_find_ptr_default(model, "tas", "broken"), "broken")) {
		opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	}
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (ym_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;

	//This must happen after the 68K context has been allocated
	for (int i = 0; i < map_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}

	if (gen->mapper_type == MAPPER_SEGA) {
		//initialize bank registers
		for (int i = 1; i < sizeof(gen->bank_regs); i++)
		{
			gen->bank_regs[i] = i;
		}
	}
	gen->reset_cycle = CYCLE_NEVER;

	set_audio_config(gen);
	return gen;
}

genesis_context *alloc_config_genesis_cdboot(system_media *media, uint32_t system_opts, uint8_t force_region)
{
	tern_node *rom_db = get_rom_db();
	rom_info info = configure_rom(rom_db, media->buffer, media->size, NULL, 0, base_map, base_chunks);
	if (media->size > 0x20B) {
		//Use a byte in the security code region that's unique across all 3 regions
		//since it's more reliable than the official header field for this
		uint8_t *bytes = media->buffer;
		switch (bytes[0x20B])
		{
		case 0x7A:
			info.regions = REGION_U;
			break;
		case 0xA1:
			info.regions = REGION_J;
			break;
		case 0x64:
			info.regions = REGION_E;
			break;
		}
	}

	segacd_context *cd = alloc_configure_segacd(media, system_opts, force_region, &info);
	genesis_context *gen = shared_init(system_opts, &info, force_region);
	gen->cart = gen->lock_on = NULL;
	gen->save_storage = NULL;
	gen->save_type = SAVE_NONE;
	gen->version_reg &= ~NO_DISK;

	gen->expansion = cd;
	cd->m68k->options->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address_sub.log", "w") : NULL;
	gen->version_reg &= ~NO_DISK;
	cd->genesis = gen;
	setup_io_devices(config, &info, &gen->io);

	uint32_t cd_chunks;
	memmap_chunk *cd_map = segacd_main_cpu_map(gen->expansion, 0, &cd_chunks);
	memmap_chunk *map = malloc(sizeof(memmap_chunk) * (cd_chunks + base_chunks));
	memcpy(map, cd_map, sizeof(memmap_chunk) * cd_chunks);
	memcpy(map + cd_chunks, base_map, sizeof(memmap_chunk) * base_chunks);
	map[cd_chunks].buffer = gen->work_ram;
	uint32_t num_chunks = cd_chunks + base_chunks;

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, num_chunks, MCLKS_PER_68K, sync_components);
	//TODO: make this configurable
	opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;

	//This must happen after the 68K context has been allocated
	for (int i = 0; i < num_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}
	gen->header.type = SYSTEM_SEGACD;

	set_audio_config(gen);
	return gen;
}
