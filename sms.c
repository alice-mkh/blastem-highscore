#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "sms.h"
#include "blastem.h"
#include "render.h"
#include "util.h"
#include "debug.h"

static void *memory_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location & 1) {
		sms->io.ports[0].control = ~(value << 5 & 0x60);
		sms->io.ports[1].control = ~(value << 3 & 0x60);
		io_data_write(sms->io.ports, value << 1, z80->current_cycle);
		io_data_write(sms->io.ports + 1, value >> 1, z80->current_cycle);
	} else {
		//TODO: memory control write
	}
	return vcontext;
}

static uint8_t hv_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->current_cycle);
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
	psg_run(sms->psg, z80->current_cycle);
	psg_write(sms->psg, value);
	return vcontext;
}

static void update_interrupts(sms_context *sms)
{
	uint32_t vint = vdp_next_vint(sms->vdp);
	uint32_t hint = vdp_next_hint(sms->vdp);
	sms->z80->int_pulse_start = vint < hint ? vint : hint;
}

static uint8_t vdp_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->current_cycle);
	if (location & 1) {
		uint8_t ret = vdp_control_port_read(sms->vdp);
		sms->vdp->flags2 &= ~(FLAG2_VINT_PENDING|FLAG2_HINT_PENDING);
		update_interrupts(sms);
		return ret;
	} else {
		return vdp_data_port_read(sms->vdp);
	}
}

static void *vdp_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	vdp_run_context(sms->vdp, z80->current_cycle);
	if (location & 1) {
		vdp_control_port_write_pbc(sms->vdp, value);
		update_interrupts(sms);
	} else {
		vdp_data_port_write_pbc(sms->vdp, value);
	}
	return vcontext;
}

static uint8_t io_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	if (location == 0xC0 || location == 0xDC) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->current_cycle);
		uint8_t port_b = io_data_read(sms->io.ports, z80->current_cycle);
		return (port_a & 0x3F) | (port_b << 6);
	}
	if (location == 0xC1 || location == 0xDD) {
		uint8_t port_a = io_data_read(sms->io.ports, z80->current_cycle);
		uint8_t port_b = io_data_read(sms->io.ports, z80->current_cycle);
		return (port_a & 0x40) | (port_b >> 2 & 0xF) | (port_b << 1 & 0x80) | 0x10;
	}
	return 0xFF;
}

static void *mapper_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	sms_context *sms = z80->system;
	sms->ram[location & (sizeof(sms->ram)-1)] = value;
	switch (location & 3)
	{
	case 0:
		//TODO: implement me
		break;
	case 1:
		z80->mem_pointers[0] = sms->rom + (value << 14 & (sms->rom_size-1)) + 0x400;
		//TODO: invalidate translated code in range 0x400-0x4000
		break;
	case 2:
		z80->mem_pointers[1] = sms->rom + (value << 14 & (sms->rom_size-1));
		//TODO: invalidate translated code in range 0x4000-0x8000
		break;
	case 3:
		z80->mem_pointers[2] = sms->rom + (value << 14 & (sms->rom_size-1));
		//TODO: invalidate translated code in range 0x8000-0xC000
		break;
	}
	return vcontext;
}

static memmap_chunk io_map[] = {
	{0x00, 0x40, 0xFF, 0, 0, 0, NULL, NULL, NULL, NULL,     memory_io_write},
	{0x40, 0x80, 0xFF, 0, 0, 0, NULL, NULL, NULL, hv_read,  sms_psg_write},
	{0x80, 0xC0, 0xFF, 0, 0, 0, NULL, NULL, NULL, vdp_read, vdp_write},
	{0xC0, 0x100,0xFF, 0, 0, 0, NULL, NULL, NULL, io_read,  NULL}
};

static void set_speed_percent(system_header * system, uint32_t percent)
{
	sms_context *context = (sms_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;

	psg_adjust_master_clock(context->psg, context->master_clock);
}

static void run_sms(system_header *system)
{
	render_disable_ym();
	sms_context *sms = (sms_context *)system;
	uint32_t target_cycle = sms->z80->current_cycle + 3420*262;
	while (!sms->should_return)
	{
		if (system->enter_debugger && sms->z80->pc) {
			system->enter_debugger = 0;
			zdebugger(sms->z80, sms->z80->pc);
		}
		z80_run(sms->z80, target_cycle);
		target_cycle = sms->z80->current_cycle;
		vdp_run_context(sms->vdp, target_cycle);
		psg_run(sms->psg, target_cycle);
		target_cycle += 3420*262;
		if (target_cycle > 0x10000000) {
			uint32_t adjust = sms->z80->current_cycle - 3420*262*2;
			io_adjust_cycles(sms->io.ports, sms->z80->current_cycle, adjust);
			io_adjust_cycles(sms->io.ports+1, sms->z80->current_cycle, adjust);
			z80_adjust_cycles(sms->z80, adjust);
			vdp_adjust_cycles(sms->vdp, adjust);
			sms->psg->cycles -= adjust;
			target_cycle -= adjust;
		}
	}
	sms->should_return = 0;
	render_enable_ym();
}

static void start_sms(system_header *system, char *statefile)
{
	sms_context *sms = (sms_context *)system;
	set_keybindings(&sms->io);
	
	if (system->enter_debugger) {
		system->enter_debugger = 0;
		zinsert_breakpoint(sms->z80, 0, (uint8_t *)zdebugger);
	}
	
	z80_assert_reset(sms->z80, 0);
	z80_clear_reset(sms->z80, 128*15);
	
	run_sms(system);
}

static void free_sms(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	vdp_free(sms->vdp);
	z80_options_free(sms->z80->options);
	free(sms->z80);
	psg_free(sms->psg);
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
}

static void inc_debug_mode(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->vdp->debug++;
	if (sms->vdp->debug == 7) {
		sms->vdp->debug = 0;
	}
}

static void inc_debug_pal(system_header *system)
{
	sms_context *sms = (sms_context *)system;
	sms->vdp->debug_pal++;
	if (sms->vdp->debug_pal == 4) {
		sms->vdp->debug_pal = 0;
	}
}

sms_context *alloc_configure_sms(void *rom, uint32_t rom_size, void *extra_rom, uint32_t extra_rom_size, uint32_t opts, uint8_t force_region, rom_info *info_out)
{
	memset(info_out, 0, sizeof(*info_out));
	sms_context *sms = calloc(1, sizeof(sms_context));
	uint32_t orig_size = rom_size;
	rom_size = nearest_pow2(rom_size);
	memmap_chunk memory_map[6];
	if (orig_size > 0xC000)  {
		info_out->map_chunks = 6;
		uint8_t *ram_reg_overlap = sms->ram + sizeof(sms->ram) - 4;
		memory_map[0] = (memmap_chunk){0x0000, 0x0400,  0xFFFF,             0, 0, MMAP_READ,                      rom,      NULL, NULL, NULL, NULL};
		memory_map[1] = (memmap_chunk){0x0400, 0x4000,  0xFFFF,             0, 0, MMAP_READ|MMAP_PTR_IDX,         NULL,     NULL, NULL, NULL, NULL};
		memory_map[2] = (memmap_chunk){0x4000, 0x8000,  0x3FFF,             0, 1, MMAP_READ|MMAP_PTR_IDX,         NULL,     NULL, NULL, NULL, NULL};
		memory_map[3] = (memmap_chunk){0x8000, 0xC000,  0x3FFF,             0, 2, MMAP_READ|MMAP_PTR_IDX,         NULL,     NULL, NULL, NULL, NULL};
		memory_map[4] = (memmap_chunk){0xC000, 0xFFFC,  sizeof(sms->ram)-1, 0, 0, MMAP_READ|MMAP_WRITE|MMAP_CODE, sms->ram, NULL, NULL, NULL, NULL};
		memory_map[5] = (memmap_chunk){0xFFFC, 0x10000, 0xFFFF,             0, 0, MMAP_READ,                      ram_reg_overlap, NULL, NULL, NULL, mapper_write};
	} else {
		info_out->map_chunks = 2;
		memory_map[0] = (memmap_chunk){0x0000, 0xC000,  rom_size-1,         0, 0, MMAP_READ,                      rom,      NULL, NULL, NULL, NULL};
		memory_map[1] = (memmap_chunk){0xC000, 0x10000, sizeof(sms->ram)-1, 0, 0, MMAP_READ|MMAP_WRITE|MMAP_CODE, sms->ram, NULL, NULL, NULL, NULL};
	};
	info_out->map = malloc(sizeof(memmap_chunk) * info_out->map_chunks);
	memcpy(info_out->map, memory_map, sizeof(memmap_chunk) * info_out->map_chunks);
	z80_options *zopts = malloc(sizeof(z80_options));
	init_z80_opts(zopts, info_out->map, info_out->map_chunks, io_map, 4, 15, 0xFF);
	sms->z80 = init_z80_context(zopts);
	sms->z80->system = sms;
	
	sms->rom = rom;
	sms->rom_size = rom_size;
	if (info_out->map_chunks > 2) {
		sms->z80->mem_pointers[0] = sms->rom;
		sms->z80->mem_pointers[1] = sms->rom + 0x4000;
		sms->z80->mem_pointers[2] = sms->rom + 0x8000;
	}
	
	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0").ptrval;
	uint32_t lowpass_cutoff = lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : 3390;
	
	//TODO: Detect region and pick master clock based off of that
	sms->normal_clock = sms->master_clock = 53693175;
	
	sms->psg = malloc(sizeof(psg_context));
	psg_init(sms->psg, render_sample_rate(), sms->master_clock, 15*16, render_audio_buffer(), lowpass_cutoff);
	
	sms->vdp = malloc(sizeof(vdp_context));
	init_vdp_context(sms->vdp, 0);
	sms->vdp->system = &sms->header;
	
	info_out->save_type = SAVE_NONE;
	info_out->name = strdup("Master System Game");
	
	setup_io_devices(config, info_out, &sms->io);
	
	sms->header.set_speed_percent = set_speed_percent;
	sms->header.start_context = start_sms;
	sms->header.resume_context = run_sms;
	//TODO: Fill in NULL values
	sms->header.load_save = NULL;
	sms->header.persist_save = NULL;
	sms->header.free_context = free_sms;
	sms->header.get_open_bus_value = get_open_bus_value;
	sms->header.request_exit = request_exit;
	sms->header.inc_debug_mode = inc_debug_mode;
	sms->header.inc_debug_pal = inc_debug_pal;
	
	return sms;
}