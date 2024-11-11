#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "config.h"
#include "sms.h"
#include "blastem.h"
#include "render.h"
#include "util.h"
#include "debug.h"
#include "saves.h"
#include "bindings.h"

#ifdef NEW_CORE
#define Z80_CYCLE cycles
#define Z80_OPTS opts
#define z80_handle_code_write(...)
#else
#define Z80_CYCLE current_cycle
#define Z80_OPTS options
#endif

static void *memory_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location & 1) {
		uint8_t fuzzy_ctrl_0 = sms->io.ports[0].control, fuzzy_ctrl_1 = sms->io.ports[1].control;
		io_control_write(sms->io.ports, (~value) << 5 & 0x60, z80->Z80_CYCLE);
		fuzzy_ctrl_0 |= sms->io.ports[0].control;
		io_control_write(sms->io.ports+1, (~value) << 3 & 0x60, z80->Z80_CYCLE);
		fuzzy_ctrl_1 |= sms->io.ports[1].control;
		if (
			(fuzzy_ctrl_0 & 0x40 & (sms->io.ports[0].output ^ (value << 1)) & (value << 1))
			|| (fuzzy_ctrl_0 & 0x40 & (sms->io.ports[1].output ^ (value >> 1)) & (value >> 1))
		) {
			//TH is an output and it went from 0 -> 1
			vdp_run_context(sms->vdp, z80->Z80_CYCLE);
			vdp_latch_hv(sms->vdp);
		}
		io_data_write(sms->io.ports, value << 1, z80->Z80_CYCLE);
		io_data_write(sms->io.ports + 1, value >> 1, z80->Z80_CYCLE);
	} else {
		//TODO: memory control write
	}
	return vcontext;
}

static uint8_t hv_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->Z80_CYCLE);
	uint16_t hv = vdp_hv_counter_read(sms->vdp);
	if (location & 1) {
		return hv;
	} else {
		return hv >> 8;
	}
}

static void *sms_psg_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	psg_run(sms->psg, z80->Z80_CYCLE);
	psg_write(sms->psg, value);
	return vcontext;
}

static void update_interrupts(sms_context *sms)
{
	uint32_t vint = vdp_next_vint(sms->vdp);
	uint32_t hint = vdp_next_hint(sms->vdp);
#ifdef NEW_CORE
	sms->z80->int_cycle = vint < hint ? vint : hint;
	z80_sync_cycle(sms->z80, sms->z80->sync_cycle);
#else
	sms->z80->int_pulse_start = vint < hint ? vint : hint;
#endif
}

static uint8_t vdp_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->Z80_CYCLE);
	if (location & 1) {
		uint8_t ret = vdp_control_port_read(sms->vdp);
		sms->vdp->flags2 &= ~(FLAG2_VINT_PENDING|FLAG2_HINT_PENDING);
		update_interrupts(sms);
		return ret;
	} else {
		return vdp_data_port_read_pbc(sms->vdp);
	}
}

static void *vdp_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location & 1) {
		vdp_run_context_full(sms->vdp, z80->Z80_CYCLE);
		vdp_control_port_write_pbc(sms->vdp, value);
		update_interrupts(sms);
	} else {
		vdp_run_context(sms->vdp, z80->Z80_CYCLE);
		vdp_data_port_write_pbc(sms->vdp, value);
	}
	return vcontext;
}

static uint8_t io_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location == 0xC0 || location == 0xDC) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->Z80_CYCLE);
		uint8_t port_b = io_data_read(sms->io.ports+1, z80->Z80_CYCLE);
		return (port_a & 0x3F) | (port_b << 6);
	}
	if (location == 0xC1 || location == 0xDD) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->Z80_CYCLE);
		uint8_t port_b = io_data_read(sms->io.ports+1, z80->Z80_CYCLE);
		return (port_a & 0x40) | (port_b >> 2 & 0xF) | (port_b << 1 & 0x80) | 0x10;
	}
	return 0xFF;
}

static void i8255_output_updated(i8255 *ppi, uint32_t cycle, uint32_t port, uint8_t data)
{
	if (port == 2) {
		sms_context *sms = ppi->system;
		sms->kb_mux = data & 0x7;
	}
}

static uint8_t i8255_input_poll(i8255 *ppi, uint32_t cycle, uint32_t port)
{
	if (port > 1) {
		return 0xFF;
	}
	sms_context *sms = ppi->system;
	if (sms->kb_mux == 7) {
		if (port) {
			//TODO: cassette-in
			//TODO: printer port BUSY/FAULT
			uint8_t port_b = io_data_read(sms->io.ports+1, cycle);
			return (port_b >> 2 & 0xF) | 0x10;
		} else {
			uint8_t port_a = io_data_read(sms->io.ports, cycle);
			uint8_t port_b = io_data_read(sms->io.ports+1, cycle);
			return (port_a & 0x3F) | (port_b << 6);
		}
	}
	//TODO: keyboard matrix ghosting
	if (port) {
		//TODO: cassette-in
		//TODO: printer port BUSY/FAULT
		return (sms->keystate[sms->kb_mux] >> 8) | 0x10;
	}
	return sms->keystate[sms->kb_mux];
}

static void update_mem_map(uint32_t location, sms_context *sms, uint8_t value)
{
	z80_context *z80 = sms->z80;
	void *old_value;
	if (location) {
		uint32_t idx = location - 1;
		old_value = z80->mem_pointers[idx];
		z80->mem_pointers[idx] = sms->rom + (value << 14 & (sms->rom_size-1));
		if (old_value != z80->mem_pointers[idx]) {
			//invalidate any code we translated for the relevant bank
			z80_invalidate_code_range(z80, idx ? idx * 0x4000 : 0x400, idx * 0x4000 + 0x4000);
		}
	} else {
		old_value = z80->mem_pointers[2];
		if (value & 8) {
			//cartridge RAM is enabled
			z80->mem_pointers[2] = sms->cart_ram + (value & 4 ? (SMS_CART_RAM_SIZE/2) : 0);
		} else {
			//cartridge RAM is disabled
			z80->mem_pointers[2] = sms->rom + (sms->bank_regs[3] << 14 & (sms->rom_size-1));
		}
		if (old_value != z80->mem_pointers[2]) {
			//invalidate any code we translated for the relevant bank
			z80_invalidate_code_range(z80, 0x8000, 0xC000);
		}
	}
}

void *sms_sega_mapper_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	location &= 3;
	sms->ram[0x1FFC + location] = value;
	sms->bank_regs[location] = value;
	update_mem_map(location, sms, value);
	return vcontext;
}

void *sms_cart_ram_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (sms->bank_regs[0] & 8) {
		//cartridge RAM is enabled
		location &= 0x3FFF;
		z80->mem_pointers[2][location] = value;
		z80_handle_code_write(0x8000 + location, z80);
	}
	return vcontext;
}

static z80_context *codemasters_write(uint8_t bank, z80_context *z80, uint8_t value)
{
	sms_context *sms = z80->system;
	if (value != sms->bank_regs[bank]) {
		sms->bank_regs[bank] = value;
		value &= 0x7F;
		z80->mem_pointers[bank] = sms->rom + (value << 14 & (sms->rom_size-1));
		z80_invalidate_code_range(z80, bank * 0x4000, bank * 0x4000 + 0x4000);
	}
	return z80;
}

void *sms_codemasters_bank0_write(uint32_t location, void *vcontext, uint8_t value)
{
	return codemasters_write(0, vcontext, value);
}

void *sms_codemasters_bank1_write(uint32_t location, void *vcontext, uint8_t value)
{
	return codemasters_write(1, vcontext, value);
}

void *sms_codemasters_bank2_write(uint32_t location, void *vcontext, uint8_t value)
{
	//TODO: Handle Ernie Els Golf cart RAM
	return codemasters_write(2, vcontext, value);
}

uint8_t debug_commands(system_header *system, char *input_buf)
{
	sms_context *sms = (sms_context *)system;
	switch(input_buf[0])
	{
	case 'v':
		if (input_buf[1] == 'r') {
			vdp_print_reg_explain(sms->vdp);
		} else if (input_buf[1] == 's') {
			vdp_print_sprite_table(sms->vdp);
		} else {
			return 0;
		}
		break;
	}
	return 1;
}

static uint8_t gg_io_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (!location) {
		return sms->start_button_region;
	} else {
		//TODO: implement link port
		return 0xFF;
	}
}

static void *gg_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	//TODO: implement link port
	return vcontext;
}
static void *psg_pan_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	psg_run(sms->psg, z80->Z80_CYCLE);
	sms->psg->pan = value;
	return vcontext;
}

static void *ppi_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	i8255_write(location, sms->i8255, value, z80->Z80_CYCLE);
	return vcontext;
}

static uint8_t ppi_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	return i8255_read(location, sms->i8255, z80->Z80_CYCLE);
}

static void *all_write(uint32_t location, void *vcontext, uint8_t value)
{
	vdp_write(location, vcontext, value);
	sms_psg_write(location, vcontext, value);
	return ppi_write(location, vcontext, value);
}

static uint8_t ppi_vdp_read(uint32_t location, void *vcontext)
{
	//TODO: "corrupt" PPI value by VDP value
	vdp_read(location, vcontext);
	return ppi_read(location, vcontext);
}

static void *vdp_psg_write(uint32_t location, void *vcontext, uint8_t value)
{
	vdp_write(location, vcontext, value);
	return sms_psg_write(location, vcontext, value);
}

static void *ppi_psg_write(uint32_t location, void *vcontext, uint8_t value)
{
	vdp_write(location, vcontext, value);
	return ppi_write(location, vcontext, value);
}

static void *ppi_vdp_write(uint32_t location, void *vcontext, uint8_t value)
{
	vdp_write(location, vcontext, value);
	return ppi_write(location, vcontext, value);
}

static memmap_chunk io_map[] = {
	{0x00, 0x40, 0xFF, .write_8 = memory_io_write},
	{0x40, 0x80, 0xFF, .read_8 = hv_read, .write_8 = sms_psg_write},
	{0x80, 0xC0, 0xFF, .read_8 = vdp_read, .write_8 = vdp_write},
	{0xC0, 0x100,0xFF, .read_8 = io_read}
};

static memmap_chunk io_gg[] = {
	{0x00, 0x06, 0xFF, .read_8 = gg_io_read, .write_8 = gg_io_write},
	{0x06, 0x07, 0xFF, .write_8 = psg_pan_write},
	{0x08, 0x40, 0xFF, .write_8 = memory_io_write},
	{0x40, 0x80, 0xFF, .read_8 = hv_read, .write_8 = sms_psg_write},
	{0x80, 0xC0, 0xFF, .read_8 = vdp_read, .write_8 = vdp_write},
	{0xC0, 0x100,0xFF, .read_8 = io_read}
};

static memmap_chunk io_sc[] = {
	{0x00, 0x20, 0x03, .read_8 = ppi_vdp_read, .write_8 = all_write},
	{0x20, 0x40, 0xFF, .read_8 = vdp_read, .write_8 = vdp_psg_write},
	{0x40, 0x60, 0x03, .read_8 = ppi_read, .write_8 = ppi_psg_write},
	{0x60, 0x80, 0xFF, .write_8 = sms_psg_write},
	{0x80, 0xA0, 0x03, .read_8 = ppi_vdp_read, .write_8 = ppi_vdp_write},
	{0xA0, 0xC0, 0xFF, .read_8 = vdp_read, .write_8 = vdp_write},
	{0xD0, 0x100, 0x03, .read_8 = ppi_read, .write_8 = ppi_write}
};

static void set_speed_percent(system_header * system, uint32_t percent)
{
	sms_context *context = (sms_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;

	psg_adjust_master_clock(context->psg, context->master_clock);
}

void sms_serialize(sms_context *sms, serialize_buffer *buf)
{
	start_section(buf, SECTION_Z80);
	z80_serialize(sms->z80, buf);
	end_section(buf);

	start_section(buf, SECTION_VDP);
	vdp_serialize(sms->vdp, buf);
	end_section(buf);

	start_section(buf, SECTION_PSG);
	psg_serialize(sms->psg, buf);
	end_section(buf);

	start_section(buf, SECTION_SEGA_IO_1);
	io_serialize(sms->io.ports, buf);
	end_section(buf);

	start_section(buf, SECTION_SEGA_IO_2);
	io_serialize(sms->io.ports + 1, buf);
	end_section(buf);

	start_section(buf, SECTION_MAIN_RAM);
	save_int8(buf, sizeof(sms->ram) / 1024);
	save_buffer8(buf, sms->ram, sizeof(sms->ram));
	end_section(buf);

	start_section(buf, SECTION_MAPPER);
	save_int8(buf, 1);//mapper type, 1 for Sega mapper
	save_buffer8(buf, sms->bank_regs, sizeof(sms->bank_regs));
	end_section(buf);

	start_section(buf, SECTION_CART_RAM);
	save_int8(buf, SMS_CART_RAM_SIZE / 1024);
	save_buffer8(buf, sms->cart_ram, SMS_CART_RAM_SIZE);
	end_section(buf);
}

static uint8_t *serialize(system_header *sys, size_t *size_out)
{
	sms_context *sms = (sms_context *)sys;
	serialize_buffer state;
	init_serialize(&state);
	sms_serialize(sms, &state);
	if (size_out) {
		*size_out = state.size;
	}
	return state.data;
}

static void ram_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > sizeof(sms->ram)) {
		fatal_error("State has a RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, sms->ram, ram_size);
}

static void cart_ram_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > SMS_CART_RAM_SIZE) {
		fatal_error("State has a cart RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, sms->cart_ram, ram_size);
}

static void mapper_deserialize(deserialize_buffer *buf, void *vsms)
{
	sms_context *sms = vsms;
	uint8_t mapper_type = load_int8(buf);
	if (mapper_type != 1) {
		warning("State contains an unrecognized mapper type %d, it may be from a newer version of BlastEm\n", mapper_type);
		return;
	}
	for (int i = 0; i < sizeof(sms->bank_regs); i++)
	{
		sms->bank_regs[i] = load_int8(buf);
		update_mem_map(i, sms, sms->bank_regs[i]);
	}
}

void sms_deserialize(deserialize_buffer *buf, sms_context *sms)
{
	register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = sms->z80}, SECTION_Z80);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = sms->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = sms->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = sms->io.ports}, SECTION_SEGA_IO_1);
	register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = sms->io.ports + 1}, SECTION_SEGA_IO_2);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = sms}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = mapper_deserialize, .data = sms}, SECTION_MAPPER);
	register_section_handler(buf, (section_handler){.fun = cart_ram_deserialize, .data = sms}, SECTION_CART_RAM);
	//TODO: cart RAM
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	z80_invalidate_code_range(sms->z80, 0xC000, 0x10000);
	if (sms->bank_regs[0] & 8) {
		//cart RAM is enabled, invalidate the region in case there is any code there
		z80_invalidate_code_range(sms->z80, 0x8000, 0xC000);
	}
	free(buf->handlers);
	buf->handlers = NULL;
}

static void deserialize(system_header *sys, uint8_t *data, size_t size)
{
	sms_context *sms = (sms_context *)sys;
	deserialize_buffer buffer;
	init_deserialize(&buffer, data, size);
	sms_deserialize(&buffer, sms);
}

static void save_state(sms_context *sms, uint8_t slot)
{
	char *save_path = get_slot_name(&sms->header, slot, "state");
	serialize_buffer state;
	init_serialize(&state);
	sms_serialize(sms, &state);
	save_to_file(&state, save_path);
	printf("Saved state to %s\n", save_path);
	free(save_path);
	free(state.data);
}

static uint8_t load_state_path(sms_context *sms, char *path)
{
	deserialize_buffer state;
	uint8_t ret;
	if ((ret = load_from_file(&state, path))) {
		sms_deserialize(&state, sms);
		free(state.data);
		printf("Loaded %s\n", path);
	}
	return ret;
}

static uint8_t load_state(system_header *system, uint8_t slot)
{
	sms_context *sms = (sms_context *)system;
	char *statepath = get_slot_name(system, slot, "state");
	uint8_t ret;
#ifndef NEW_CORE
	if (!sms->z80->native_pc) {
		ret = get_modification_time(statepath) != 0;
		if (ret) {
			system->delayed_load_slot = slot + 1;
		}
		goto done;

	}
#endif
	ret = load_state_path(sms, statepath);
done:
	free(statepath);
	return ret;
}

static void run_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	uint32_t target_cycle = sms->z80->Z80_CYCLE + 3420*16;
	//TODO: PAL support
	if (sms->vdp->type == VDP_GAMEGEAR) {
		render_set_video_standard(VID_GAMEGEAR);
	} else {
		render_set_video_standard(VID_NTSC);
	}
	while (!sms->should_return)
	{
		if (system->delayed_load_slot) {
			load_state(system, system->delayed_load_slot - 1);
			system->delayed_load_slot = 0;

		}
		if (sms->vdp->frame != sms->last_frame) {
#ifndef IS_LIB
			if (sms->psg->scope) {
				scope_render(sms->psg->scope);
			}
#endif
			uint32_t elapsed = sms->vdp->frame - sms->last_frame;
			sms->last_frame = sms->vdp->frame;
			if (system->enter_debugger_frames) {
				if (elapsed >= system->enter_debugger_frames) {
					system->enter_debugger_frames = 0;
					system->enter_debugger = 1;
				} else {
					system->enter_debugger_frames -= elapsed;
				}
			}

			if(exit_after){
				if (elapsed >= exit_after) {
					exit(0);
				} else {
					exit_after -= elapsed;
				}
			}
		}
#ifndef NEW_CORE
		if ((system->enter_debugger || sms->z80->wp_hit) && sms->z80->pc) {
			if (!sms->z80->wp_hit) {
				system->enter_debugger = 0;
			}
#ifndef IS_LIB
			zdebugger(sms->z80, sms->z80->pc);
#endif
		}
#endif
#ifdef NEW_CORE
		if (sms->z80->nmi_cycle == CYCLE_NEVER) {
#else
		if (sms->z80->nmi_start == CYCLE_NEVER) {
#endif
			uint32_t nmi = vdp_next_nmi(sms->vdp);
			if (nmi != CYCLE_NEVER) {
				z80_assert_nmi(sms->z80, nmi);
			}
		}

#ifndef NEW_CORE
		if (system->enter_debugger || sms->z80->wp_hit) {
			target_cycle = sms->z80->Z80_CYCLE + 1;
		}
#endif
		z80_run(sms->z80, target_cycle);
		if (sms->z80->reset) {
			z80_clear_reset(sms->z80, sms->z80->Z80_CYCLE + 128*15);
		}
		target_cycle = sms->z80->Z80_CYCLE;
		vdp_run_context(sms->vdp, target_cycle);
		psg_run(sms->psg, target_cycle);

		if (system->save_state) {
			while (!sms->z80->pc) {
				//advance Z80 to an instruction boundary
				z80_run(sms->z80, sms->z80->Z80_CYCLE + 1);
			}
			save_state(sms, system->save_state - 1);
			system->save_state = 0;
		}

		target_cycle += 3420*16;
		if (target_cycle > 0x10000000) {
			uint32_t adjust = sms->z80->Z80_CYCLE - 3420*262*2;
			io_adjust_cycles(sms->io.ports, sms->z80->Z80_CYCLE, adjust);
			io_adjust_cycles(sms->io.ports+1, sms->z80->Z80_CYCLE, adjust);
			z80_adjust_cycles(sms->z80, adjust);
			vdp_adjust_cycles(sms->vdp, adjust);
			sms->psg->cycles -= adjust;
			if (sms->psg->vgm) {
				vgm_adjust_cycles(sms->psg->vgm, adjust);
			}
			target_cycle -= adjust;
		}
	}
	if (sms->header.force_release || render_should_release_on_exit()) {
		bindings_release_capture();
		vdp_release_framebuffer(sms->vdp);
		render_pause_source(sms->psg->audio);
	}
	sms->should_return = 0;
}

static void resume_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	if (sms->header.force_release || render_should_release_on_exit()) {
		sms->header.force_release = 0;
		bindings_reacquire_capture();
		vdp_reacquire_framebuffer(sms->vdp);
		render_resume_source(sms->psg->audio);
	}
	run_sms(system);
}

static void start_sms(system_header *system, char *statefile)
{
	sms_context *sms = (sms_context *)system;

	z80_assert_reset(sms->z80, 0);
	z80_clear_reset(sms->z80, 128*15);

	if (statefile) {
		load_state_path(sms, statefile);
	}

	if (system->enter_debugger) {
		system->enter_debugger = 0;
#ifndef IS_LIB
		zinsert_breakpoint(sms->z80, sms->z80->pc, (uint8_t *)zdebugger);
#endif
	}

	run_sms(system);
}

static void soft_reset(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	z80_assert_reset(sms->z80, sms->z80->Z80_CYCLE);
#ifndef NEW_CORE
	sms->z80->target_cycle = sms->z80->sync_cycle = sms->z80->Z80_CYCLE;
#endif
}

static void free_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	vdp_free(sms->vdp);
	z80_options_free(sms->z80->Z80_OPTS);
	free(sms->z80);
	psg_free(sms->psg);
	free(sms->i8255);
	free(sms);
}

static uint16_t get_open_bus_value(system_header *system)
{
	return 0xFFFF;
}

static void request_exit(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->should_return = 1;
#ifndef NEW_CORE
	sms->z80->target_cycle = sms->z80->sync_cycle = sms->z80->Z80_CYCLE;
#endif
}

static void inc_debug_mode(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	vdp_inc_debug_mode(sms->vdp);
}

static void load_save(system_header *system)
{
	//TODO: Implement me
}

static void persist_save(system_header *system)
{
	//TODO: Implement me
}

static void gamepad_down(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	sms_context *sms = (sms_context *)system;
	if (gamepad_num == GAMEPAD_MAIN_UNIT) {
		if (button == MAIN_UNIT_PAUSE) {
			vdp_pbc_pause(sms->vdp);
		}
	} else if (sms->vdp->type == VDP_GAMEGEAR && gamepad_num == 1 && button == BUTTON_START) {
		sms->start_button_region &= 0x7F;
	} else {
		io_gamepad_down(&sms->io, gamepad_num, button);
	}
}

static void gamepad_up(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	sms_context *sms = (sms_context *)system;
	if (sms->vdp->type == VDP_GAMEGEAR && gamepad_num == 1 && button == BUTTON_START) {
		sms->start_button_region |= 0x80;
	} else {
		io_gamepad_up(&sms->io, gamepad_num, button);
	}
}

static void mouse_down(system_header *system, uint8_t mouse_num, uint8_t button)
{
	sms_context *sms = (sms_context *)system;
	io_mouse_down(&sms->io, mouse_num, button);
}

static void mouse_up(system_header *system, uint8_t mouse_num, uint8_t button)
{
	sms_context *sms = (sms_context *)system;
	io_mouse_up(&sms->io, mouse_num, button);
}

static void mouse_motion_absolute(system_header *system, uint8_t mouse_num, uint16_t x, uint16_t y)
{
	sms_context *sms = (sms_context *)system;
	io_mouse_motion_absolute(&sms->io, mouse_num, x, y);
}

static void mouse_motion_relative(system_header *system, uint8_t mouse_num, int32_t x, int32_t y)
{
	sms_context *sms = (sms_context *)system;
	io_mouse_motion_relative(&sms->io, mouse_num, x, y);
}

uint16_t scancode_map[0x90] = {
	[0x1C] = 0x0004,//A
	[0x32] = 0x4008,//B
	[0x21] = 0x2008,//C
	[0x23] = 0x2004,//D
	[0x24] = 0x2002,//E
	[0x2B] = 0x3004,//F
	[0x34] = 0x4004,//G
	[0x33] = 0x5004,//H
	[0x43] = 0x0080,//I
	[0x3B] = 0x6004,//J
	[0x42] = 0x0040,//K
	[0x4B] = 0x1040,//L
	[0x3A] = 0x6008,//M
	[0x31] = 0x5008,//N
	[0x44] = 0x1080,//O
	[0x4D] = 0x2080,//P
	[0x15] = 0x0002,//Q
	[0x2D] = 0x3002,//R
	[0x1B] = 0x1004,//S
	[0x2C] = 0x4002,//T
	[0x3C] = 0x6002,//U
	[0x2A] = 0x3008,//V
	[0x1D] = 0x1002,//W
	[0x22] = 0x1008,//X
	[0x35] = 0x5002,//Y
	[0x1A] = 0x0008,//Z
	[0x16] = 0x0001,//1
	[0x1E] = 0x1001,//2
	[0x26] = 0x2001,//3
	[0x25] = 0x3001,//4
	[0x2E] = 0x4001,//5
	[0x36] = 0x5001,//6
	[0x3D] = 0x6001,//7
	[0x3E] = 0x0100,//8
	[0x46] = 0x1100,//9
	[0x45] = 0x2100,//0
	[0x5A] = 0x5040,//return
	[0x29] = 0x1010,//space
	[0x0D] = 0x5800,//tab mapped to FUNC
	[0x66] = 0x3010,//backspace mapped to INS/DEL
	[0x4E] = 0x3100,// -
	[0x55] = 0x4100,// = mapped to ^ based on position
	[0x54] = 0x4080,// [
	[0x5B] = 0x4040,// ]
	[0x5D] = 0x5100,// \ mapped to Yen based on position/correspondence on PC keyboards
	[0x4C] = 0x2040,// ;
	[0x52] = 0x3040,// ' mapped to : based on position
	[0x0E] = 0x3020,// ` mapped to PI because of lack of good options
	[0x41] = 0x0020,// ,
	[0x49] = 0x1020,// .
	[0x4A] = 0x2020,// /
	[0x14] = 0x6400,//lctrl mapped to ctrl
	//rctrl is default keybind for toggle keyboard capture
	//[0x18] = 0x6400,//rctrl mapped to ctrl
	[0x12] = 0x6800,//lshift mapped to shift
	[0x59] = 0x6800,//lshift mapped to shift
	[0x11] = 0x6200,//lalt mapped to GRAPH
	[0x17] = 0x6200,//ralt mapped to GRAPH
	[0x81] = 0x0010,//insert mapped to kana/dieresis key
	[0x86] = 0x5020,//left arrow
	[0x87] = 0x2010,//home mapped to HOME/CLR
	[0x88] = 0x6100,//end mapped to BREAK
	[0x89] = 0x6040,//up arrow
	[0x8A] = 0x4020,//down arrow
	[0x8D] = 0x6020,//right arrow
};

static void keyboard_down(system_header *system, uint8_t scancode)
{
	sms_context *sms = (sms_context *)system;
	io_keyboard_down(&sms->io, scancode);
	if (sms->keystate && scancode < 0x90 && scancode_map[scancode]) {
		uint16_t row = scancode_map[scancode] >> 12;
		sms->keystate[row] &= ~(scancode_map[scancode] & 0xFFF);
	}
}

static void keyboard_up(system_header *system, uint8_t scancode)
{
	sms_context *sms = (sms_context *)system;
	io_keyboard_up(&sms->io, scancode);
	if (sms->keystate && scancode < 0x90 && scancode_map[scancode]) {
		uint16_t row = scancode_map[scancode] >> 12;
		sms->keystate[row] |= scancode_map[scancode] & 0xFFF;
	}
}

static void set_gain_config(sms_context *sms)
{
	char *config_gain;
	config_gain = tern_find_path(config, "audio\0psg_gain\0", TVAL_PTR).ptrval;
	render_audio_source_gaindb(sms->psg->audio, config_gain ? atof(config_gain) : 0.0f);
}

static void config_updated(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	setup_io_devices(config, &system->info, &sms->io);
	//sample rate may have changed
	psg_adjust_master_clock(sms->psg, sms->master_clock);
}

static void toggle_debug_view(system_header *system, uint8_t debug_view)
{
#ifndef IS_LIB
	sms_context *sms = (sms_context *)system;
	if (debug_view < DEBUG_OSCILLOSCOPE) {
		vdp_toggle_debug_view(sms->vdp, debug_view);
	} else if (debug_view == DEBUG_OSCILLOSCOPE) {
		if (sms->psg->scope) {
			oscilloscope *scope = sms->psg->scope;
			sms->psg->scope = NULL;
			scope_close(scope);
		} else {
			oscilloscope *scope = create_oscilloscope();
			psg_enable_scope(sms->psg, scope, sms->normal_clock);
		}
	}
#endif
}

static void start_vgm_log(system_header *system, char *filename)
{
	sms_context *sms = (sms_context *)system;
	//TODO: 50 Hz support
	vgm_writer *vgm = vgm_write_open(filename, 60, sms->normal_clock, sms->z80->Z80_CYCLE);
	if (vgm) {
		printf("Started logging VGM to %s\n", filename);
		psg_run(sms->psg, sms->z80->Z80_CYCLE);
		psg_vgm_log(sms->psg, sms->normal_clock, vgm);
		sms->header.vgm_logging = 1;
	} else {
		printf("Failed to start logging to %s\n", filename);
	}
}

static void stop_vgm_log(system_header *system)
{
	puts("Stopped VGM log");
	sms_context *sms = (sms_context *)system;
	vgm_close(sms->psg->vgm);
	sms->psg->vgm = NULL;
	sms->header.vgm_logging = 0;
}

sms_context *alloc_configure_sms(system_media *media, uint32_t opts, uint8_t force_region)
{
	sms_context *sms = calloc(1, sizeof(sms_context));
	tern_node *rom_db = get_rom_db();
	const memmap_chunk base_map[] = {
		{0xC000, 0x10000, sizeof(sms->ram)-1, .flags = MMAP_READ|MMAP_WRITE|MMAP_CODE, .buffer = sms->ram}
	};
	sms->header.info = configure_rom_sms(rom_db, media->buffer, media->size, base_map, sizeof(base_map)/sizeof(base_map[0]));
	uint32_t rom_size = sms->header.info.rom_size;
	z80_options *zopts = malloc(sizeof(z80_options));
	tern_node *model_def;
	uint8_t is_gamegear = !strcasecmp(media->extension, "gg");
	uint8_t is_sc3000 = !strcasecmp(media->extension, "sc");
	if (is_gamegear) {
		model_def = tern_find_node(get_systems_config(), "gg");
	} else if (!strcasecmp(media->extension, "sg")) {
		model_def = tern_find_node(get_systems_config(), "sg1000");
	} else if (is_sc3000) {
		model_def = tern_find_node(get_systems_config(), "sc3000");
	} else {
		model_def = get_model(config, SYSTEM_SMS);
	}
	char *vdp_str = tern_find_ptr(model_def, "vdp");
	uint8_t vdp_type = is_gamegear ? VDP_GAMEGEAR : is_sc3000 ? VDP_TMS9918A : VDP_SMS2;
	if (vdp_str) {
		if (!strcmp(vdp_str, "sms1")) {
			vdp_type = VDP_SMS;
		} else if (!strcmp(vdp_str, "sms2")) {
			vdp_type = VDP_SMS2;
		} else if (!strcmp(vdp_str, "tms9918a")) {
			vdp_type = VDP_TMS9918A;
		} else if (!strcmp(vdp_str, "gamegear")) {
			vdp_type = VDP_GAMEGEAR;
		} else if (!strcmp(vdp_str, "genesis")) {
			vdp_type = VDP_GENESIS;
		} else {
			warning("Unrecognized VDP type %s\n", vdp_str);
		}
	}
	for (uint32_t i = 0; i < sms->header.info.map_chunks; i++)
	{
		memmap_chunk *chunk = sms->header.info.map + i;
		if ((chunk->flags == MMAP_READ) && !chunk->buffer && chunk->start > 0xC000) {
			chunk->buffer = sms->ram + ((chunk->start - 0xC000) & 0x1FFF);
		}
	}
	char *io_type = tern_find_ptr(model_def, "io");
	if (io_type) {
		if (!strcmp(io_type, "gamegear")) {
			is_gamegear = 1;
			is_sc3000 = 0;
		} else if (!strcmp(io_type, "i8255")) {
			is_gamegear = 0;
			is_sc3000 = 1;
		}
	}
	if (is_gamegear) {
		init_z80_opts(zopts, sms->header.info.map, sms->header.info.map_chunks, io_gg, 6, 15, 0xFF);
		sms->start_button_region = 0xC0;
	} else if (is_sc3000) {
		sms->keystate = calloc(sizeof(uint16_t), 7);
		memset(sms->keystate, 0xFF, sizeof(uint16_t) * 7);
		sms->i8255 = calloc(1, sizeof(i8255));
		i8255_init(sms->i8255, i8255_output_updated, i8255_input_poll);
		sms->i8255->system = sms;
		sms->kb_mux = 7;
		init_z80_opts(zopts, sms->header.info.map, sms->header.info.map_chunks, io_sc, 7, 15, 0xFF);
	} else {
		init_z80_opts(zopts, sms->header.info.map, sms->header.info.map_chunks, io_map, 4, 15, 0xFF);
	}
	sms->z80 = init_z80_context(zopts);
	sms->z80->system = sms;
	sms->z80->Z80_OPTS->gen.debug_cmd_handler = debug_commands;

	sms->rom = media->buffer;
	sms->rom_size = rom_size;
	if (sms->header.info.map_chunks > 2) {
		sms->z80->mem_pointers[0] = sms->rom;
		sms->z80->mem_pointers[1] = sms->rom + 0x4000;
		sms->z80->mem_pointers[2] = sms->rom + 0x8000;
		sms->bank_regs[1] = 0;
		sms->bank_regs[2] = 0x4000 >> 14;
		sms->bank_regs[3] = 0x8000 >> 14;
	}

	//TODO: Detect region and pick master clock based off of that
	sms->normal_clock = sms->master_clock = 53693175;

	sms->psg = malloc(sizeof(psg_context));
	psg_init(sms->psg, sms->master_clock, 15*16);

	set_gain_config(sms);

	sms->vdp = init_vdp_context(0, 0, vdp_type);
	sms->vdp->system = &sms->header;

	sms->header.info.save_type = SAVE_NONE;
	sms->header.info.name = strdup(media->name);

	tern_node *io_config_root = config;
	tern_node *sms_root = tern_find_node(config, "sms");
	if (sms_root) {
		tern_node *io = tern_find_node(sms_root, "io");
		if (io) {
			io_config_root = sms_root;
		}
	}
	setup_io_devices(io_config_root, &sms->header.info, &sms->io);
	sms->header.has_keyboard = io_has_keyboard(&sms->io) || sms->keystate;

	sms->header.set_speed_percent = set_speed_percent;
	sms->header.start_context = start_sms;
	sms->header.resume_context = resume_sms;
	sms->header.load_save = load_save;
	sms->header.persist_save = persist_save;
	sms->header.load_state = load_state;
	sms->header.free_context = free_sms;
	sms->header.get_open_bus_value = get_open_bus_value;
	sms->header.request_exit = request_exit;
	sms->header.soft_reset = soft_reset;
	sms->header.inc_debug_mode = inc_debug_mode;
	sms->header.gamepad_down = gamepad_down;
	sms->header.gamepad_up = gamepad_up;
	sms->header.mouse_down = mouse_down;
	sms->header.mouse_up = mouse_up;
	sms->header.mouse_motion_absolute = mouse_motion_absolute;
	sms->header.mouse_motion_relative = mouse_motion_relative;
	sms->header.keyboard_down = keyboard_down;
	sms->header.keyboard_up = keyboard_up;
	sms->header.config_updated = config_updated;
	sms->header.serialize = serialize;
	sms->header.deserialize = deserialize;
	sms->header.start_vgm_log = start_vgm_log;
	sms->header.stop_vgm_log = stop_vgm_log;
	sms->header.toggle_debug_view = toggle_debug_view;
	sms->header.type = SYSTEM_SMS;

	return sms;
}
