/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef Z80_TO_X86_H_
#define Z80_TO_X86_H_
#include "z80inst.h"
#include "backend.h"
#include "serialize.h"

#define ZNUM_MEM_AREAS 4
#ifdef Z80_LOG_ADDRESS
#define ZMAX_NATIVE_SIZE 255
#else
#define ZMAX_NATIVE_SIZE 160
#endif

enum {
	ZF_C = 0,
	ZF_N,
	ZF_PV,
	ZF_H,
	ZF_Z,
	ZF_S,
	ZF_XY,
	ZF_NUM
};

typedef struct {
	uint16_t start;
	uint16_t end;
	uint8_t  check_change;
} z80_watchpoint;

typedef struct z80_context z80_context;
typedef void (*z80_ctx_fun)(z80_context * context);

typedef struct {
	cpu_options     gen;
	code_ptr        save_context_scratch;
	code_ptr        load_context_scratch;
	code_ptr        native_addr;
	code_ptr        retrans_stub;
	code_ptr        do_sync;
	code_ptr        read_8;
	code_ptr        write_8;
	code_ptr        read_8_noinc;
	code_ptr        write_8_noinc;
	code_ptr        read_16;
	code_ptr        write_16_highfirst;
	code_ptr        write_16_lowfirst;
	code_ptr		read_io;
	code_ptr		write_io;
	memmap_chunk const *io_memmap;
	uint32_t        io_memmap_chunks;

	uint32_t        flags;
	uint16_t        io_address_mask;
	int8_t          regs[Z80_UNUSED];
	z80_ctx_fun     run;
} z80_options;

struct z80_context {
	void *            native_pc;
	uint16_t          sp;
	uint8_t           flags[ZF_NUM];
	uint16_t          bank_reg;
	uint8_t           regs[Z80_A+1];
	uint8_t           im;
	uint8_t           alt_regs[Z80_A+1];
	uint32_t          target_cycle;
	uint32_t          current_cycle;
	uint8_t           alt_flags[ZF_NUM];
	uint8_t *         mem_pointers[ZNUM_MEM_AREAS];
	uint8_t           iff1;
	uint8_t           iff2;
	uint16_t          scratch1;
	uint16_t          scratch2;
	void *            extra_pc;
	uint32_t          sync_cycle;
	uint32_t          int_cycle;
	z80_options *     options;
	void *            system;
	uint32_t          int_enable_cycle;
	uint16_t          pc;
	uint32_t          int_pulse_start;
	uint32_t          int_pulse_end;
	uint32_t          nmi_start;
	z80_watchpoint    *watchpoints;
	uint32_t          num_watchpoints;
	uint32_t          wp_storage;
	uint16_t          watchpoint_min;
	uint16_t          watchpoint_max;
	uint16_t          wp_hit_address;
	uint8_t           wp_hit_value;
	uint8_t           wp_old_value;
	uint8_t           wp_hit;
	uint8_t           breakpoint_flags[(16 * 1024)/sizeof(uint8_t)];
	uint8_t *         bp_handler;
	uint8_t *         bp_stub;
	uint8_t *         interp_code[256];
	z80_ctx_fun       next_int_pulse;
	uint8_t           reset;
	uint8_t           busreq;
	uint8_t           busack;
	uint8_t           int_is_nmi;
	uint8_t           im2_vector;
	uint8_t           ram_code_flags[];
};

void translate_z80_stream(z80_context * context, uint32_t address);
void init_z80_opts(z80_options * options, memmap_chunk const * chunks, uint32_t num_chunks, memmap_chunk const * io_chunks, uint32_t num_io_chunks, uint32_t clock_divider, uint32_t io_address_mask);
void z80_options_free(z80_options *opts);
z80_context * init_z80_context(z80_options * options);
code_ptr z80_get_native_address(z80_context * context, uint32_t address);
code_ptr z80_get_native_address_trans(z80_context * context, uint32_t address);
z80_context * z80_handle_code_write(uint32_t address, z80_context * context);
void z80_invalidate_code_range(z80_context *context, uint32_t start, uint32_t end);
void z80_reset(z80_context * context);
void z80_clock_divider_updated(z80_options *options);
void zinsert_breakpoint(z80_context * context, uint16_t address, uint8_t * bp_handler);
void zremove_breakpoint(z80_context * context, uint16_t address);
void z80_add_watchpoint(z80_context *context, uint16_t address, uint16_t size);
void z80_remove_watchpoint(z80_context *context, uint32_t address, uint32_t size);
void z80_run(z80_context * context, uint32_t target_cycle);
void z80_assert_reset(z80_context * context, uint32_t cycle);
void z80_clear_reset(z80_context * context, uint32_t cycle);
void z80_assert_busreq(z80_context * context, uint32_t cycle);
void z80_clear_busreq(z80_context * context, uint32_t cycle);
void z80_assert_nmi(z80_context *context, uint32_t cycle);
uint8_t z80_get_busack(z80_context * context, uint32_t cycle);
void z80_adjust_cycles(z80_context * context, uint32_t deduction);
void z80_serialize(z80_context *context, serialize_buffer *buf);
void z80_deserialize(deserialize_buffer *buf, void *vcontext);
uint32_t z80_get_instruction_start(z80_context *context, uint32_t address);

#endif //Z80_TO_X86_H_

