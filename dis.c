/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "68kinst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "vos_program_module.h"
#include "tern.h"
#include "util.h"

uint8_t visited[(16*1024*1024)/16];
uint16_t label[(16*1024*1024)/8];

void fatal_error(char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}


void visit(uint32_t address)
{
	address &= 0xFFFFFF;
	visited[address/16] |= 1 << ((address / 2) % 8);
}

void reference(uint32_t address)
{
	address &= 0xFFFFFF;
	//printf("referenced: %X\n", address);
	label[address/16] |= 1 << (address % 16);
}

uint8_t is_visited(uint32_t address)
{
	address &= 0xFFFFFF;
	return visited[address/16] & (1 << ((address / 2) % 8));
}

uint16_t is_label(uint32_t address)
{
	address &= 0xFFFFFF;
	return label[address/16] & (1 << (address % 16));
}

typedef struct {
	uint32_t num_labels;
	uint32_t storage;
	uint32_t full_address;
	char     *labels[];
} label_names;

tern_node * weak_label(tern_node * head, const char *name, uint32_t address)
{
	char key[MAX_INT_KEY_SIZE];
	tern_int_key(address & 0xFFFFFF, key);
	label_names * names = tern_find_ptr(head, key);
	if (names)
	{
		if (names->num_labels == names->storage)
		{
			names->storage = names->storage + (names->storage >> 1);
			names = realloc(names, sizeof(label_names) + names->storage * sizeof(char *));
		}
	} else {
		names = malloc(sizeof(label_names) + 4 * sizeof(char *));
		names->num_labels = 0;
		names->storage = 4;
		head = tern_insert_ptr(head, key, names);
	}
	names->labels[names->num_labels++] = strdup(name);
	names->full_address = address;
	return head;
}
tern_node *add_label(tern_node * head, const char *name, uint32_t address)
{
	reference(address);
	return weak_label(head, name, address);
}

typedef struct deferred {
	uint32_t address;
	struct deferred *next;
} deferred;

deferred * defer(uint32_t address, deferred * next)
{
	if (is_visited(address) || address & 1) {
		return next;
	}
	//printf("deferring %X\n", address);
	deferred * d = malloc(sizeof(deferred));
	d->address = address;
	d->next = next;
	return d;
}

void check_reference(m68kinst * inst, m68k_op_info * op)
{
	switch(op->addr_mode)
	{
	case MODE_PC_DISPLACE:
		reference(inst->address + 2 + op->params.regs.displacement);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		reference(op->params.immed);
		break;
	}
}

int label_fun(char *dst, uint32_t address, void * data)
{
	tern_node * labels = data;
	char key[MAX_INT_KEY_SIZE];
	label_names * names = tern_find_ptr(labels, tern_int_key(address & 0xFFFFFF, key));
	if (names)
	{
		return sprintf(dst, "%s", names->labels[0]);
	} else {
		return m68k_default_label_fun(dst, address, NULL);
	}
}

char * strip_ws(char * text)
{
	while (*text && (!isprint(*text) || isblank(*text)))
	{
		text++;
	}
	char * ret = text;
	text = ret + strlen(ret) - 1;
	while (text > ret && (!isprint(*text) || isblank(*text)))
	{
		*text = 0;
		text--;
	}
	return ret;
}

typedef struct {
	uint32_t address_off;
	uint32_t address_end;
	uint16_t *buffer;
} rom_def;

uint16_t fetch(uint32_t address, void *data)
{
	rom_def *rom = data;
	if (address >= rom->address_off && address < rom->address_end) {
		return rom->buffer[((address & 0xFFFFFF) - rom->address_off) >> 1];
	}
	return 0;
}

int main(int argc, char ** argv)
{
	long filesize;
	unsigned short *filebuf = NULL;
	char disbuf[1024];
	m68kinst instbuf;
	unsigned short * cur;
	deferred *def = NULL, *tmpd;

	uint8_t labels = 0, addr = 0, only = 0, vos = 0, reset = 0;
	tern_node * named_labels = NULL;

	uint32_t address_off = 0, address_end;
	uint8_t do_cd_labels = 0, main_cpu = 0;
	for(uint8_t opt = 2; opt < argc; ++opt) {
		if (argv[opt][0] == '-') {
			FILE * address_log;
			switch (argv[opt][1])
			{
			case 'l':
				labels = 1;
				break;
			case 'a':
				addr = 1;
				break;
			case 'o':
				only = 1;
				break;
			case 'v':
				vos = 1;
				break;
			case 'r':
				reset = 1;
				break;
			case 'c':
				do_cd_labels = 1;
				break;
			case 'm':
				main_cpu = 1;
				break;
			case 's':
				opt++;
				if (opt >= argc) {
					fputs("-s must be followed by an offset\n", stderr);
					exit(1);
				}
				address_off = strtol(argv[opt], NULL, 0);
				break;
			case 'f':
				opt++;
				if (opt >= argc) {
					fputs("-f must be followed by a filename\n", stderr);
					exit(1);
				}
				address_log = fopen(argv[opt], "r");
				if (!address_log) {
					fprintf(stderr, "Failed to open %s for reading\n", argv[opt]);
					exit(1);
				}
				while (fgets(disbuf, sizeof(disbuf), address_log)) {
				 	if (disbuf[0]) {
						char *end;
						uint32_t address = strtol(disbuf, &end, 16);
						if (address) {
							def = defer(address, def);
							reference(address);
							if (*end == '=') {
								named_labels = add_label(named_labels, strip_ws(end+1), address);
							}
						}
					}
				}
				fclose(address_log);
			}
		} else {
			char *end;
			uint32_t address = strtol(argv[opt], &end, 16);
			def = defer(address, def);
			reference(address);
			if (*end == '=') {
				named_labels = add_label(named_labels, end+1, address);
			}
		}
	}
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);

	char int_key[MAX_INT_KEY_SIZE];
	uint8_t is_scd_iso = 0;
	uint8_t has_manual_defs = !!def;
	if (vos)
	{
		vos_program_module header;
		vos_read_header(f, &header);
		vos_read_alloc_module_map(f, &header);
		address_off = header.user_boundary;
		address_end = address_off + filesize - 0x1000;
		def = defer(header.main_entry_link.code_address, def);
		named_labels = add_label(named_labels, "main_entry_link", header.main_entry_link.code_address);
		for (int i = 0; i < header.n_modules; i++)
		{
			if (!reset || header.module_map_entries[i].code_address != header.user_boundary)
			{
				def = defer(header.module_map_entries[i].code_address, def);
			}
			named_labels = add_label(named_labels, header.module_map_entries[i].name.str, header.module_map_entries[i].code_address);
		}
		fseek(f, 0x1000, SEEK_SET);
		filebuf = malloc(filesize - 0x1000);
		if (fread(filebuf, 2, (filesize - 0x1000)/2, f) != (filesize - 0x1000)/2)
		{
			fprintf(stderr, "Failure while reading file %s\n", argv[1]);
		}
		fclose(f);
		for(cur = filebuf; cur - filebuf < ((filesize - 0x1000)/2); ++cur)
		{
			*cur = (*cur >> 8) | (*cur << 8);
		}
		if (reset)
		{
			def = defer(filebuf[2] << 16 | filebuf[3], def);
			named_labels = add_label(named_labels, "reset", filebuf[2] << 16 | filebuf[3]);
		}
	} else if (filesize > 0x1000) {
		long boot_size = filesize > (32*1024) ? 32*1024 : filesize;
		filebuf = malloc(boot_size);
		if (fread(filebuf, 1, boot_size, f) != boot_size) {
			fprintf(stderr, "Failure while reading file %s\n", argv[1]);
			return 1;
		}
		is_scd_iso = !memcmp("SEGADISCSYSTEM  ", filebuf, 0x10);
		if (!is_scd_iso && !memcmp("SEGADISCSYSTEM  ", filebuf + 0x8, 0x10)) {
			is_scd_iso = 1;
			uint32_t end = 16 * 2352;
			if (end > filesize) {
				end = (filesize / 2352) * 2352;
			}
			for(uint32_t offset = 0x10, dst_offset = 0; offset < end; offset += 2352, dst_offset += 2048)
			{
				memmove(filebuf + dst_offset/2, filebuf + offset/2, 2048);
			}
			boot_size = (end / 2352) * 2048;
		}
		if (is_scd_iso) {
			fclose(f);
			for(cur = filebuf; cur - filebuf < (boot_size/2); ++cur)
			{
				*cur = (*cur >> 8) | (*cur << 8);
			}
			if (main_cpu) {
				uint32_t main_start = 0x200;
				uint32_t extra_start = filebuf[0x30/2] << 16 | filebuf[0x32/2];
				uint32_t main_end = (filebuf[0x34/2] << 16 | filebuf[0x36/2]) + extra_start;
				if (main_end > boot_size) {
					main_end = boot_size;
				}
				address_off = 0xFF0000;
				address_end = address_off + main_end-main_start;
				filebuf += main_start / 2;
				named_labels = add_label(named_labels, "start", 0xFF0000);
				if (!has_manual_defs || !only) {
					def = defer(0xFF0000, def);
				}
				uint32_t user_start;
				if (filebuf[0xA/2] == 0x57A) {
					//US
					user_start = 0xFF0584;
				} else if (filebuf[0xA/2] == 0x564) {
					//EU
					user_start = 0xFF056E;
				} else {
					//JP
					user_start = 0xFF0156;
				}
				named_labels = add_label(named_labels, "user_start", user_start);
				do_cd_labels = 1;
			} else {
				uint32_t sub_start =filebuf[0x40/2] << 16 | filebuf[0x42/2];
				uint32_t sub_end =filebuf[0x44/2] << 16 | filebuf[0x46/2];
				if (sub_start > (boot_size - 0x20)) {
					fprintf(stderr, "System Program start offset is %X, but image is only %X bytes\n", sub_start, (uint32_t)boot_size);
					return 1;
				}
				if (sub_end > boot_size) {
					sub_end = boot_size;
				}
				uint32_t offset_start = (filebuf[(sub_start + 0x18)/2] << 16 | filebuf[(sub_start + 0x1A)/2]) + sub_start;
				for(uint32_t cur = offset_start, index = 0; cur < sub_end && filebuf[cur/2]; cur+=2, ++index)
				{
					uint32_t offset = offset_start + filebuf[cur/2];
					if (offset >= boot_size) {
						break;
					}
					static const char* fixed_names[3] = {
						"init",
						"main",
						"int_2"
					};
					char namebuf[32];
					const char *name;
					if (index < 3) {
						name = fixed_names[index];
					} else {
						name = namebuf;
						sprintf(namebuf, "usercall%u", index);
					}
					uint32_t address = 0x6000 + offset - sub_start;
					named_labels = add_label(named_labels, name, address);
					if (!has_manual_defs || !only) {
						def = defer(address, def);
					}
				}

				do_cd_labels = 1;
				filebuf += sub_start / 2;
				address_off = 0x6000;
				address_end = sub_end-sub_start + address_off;
			}
		}
	}
	if (!vos && !is_scd_iso) {
		if (filebuf) {
			if (filesize > (32*1024)) {
				filebuf = realloc(filebuf, filesize);
				fseek(f, 32*1024, SEEK_SET);
				uint32_t to_read = filesize/2 - 16*1024;
				if (fread(filebuf + 16*1024, 2, to_read, f) != to_read)
				{
					fprintf(stderr, "Failure while reading file %s\n", argv[1]);
				}
			}
		} else {
			filebuf = malloc(filesize);
			if (fread(filebuf, 2, filesize/2, f) != filesize/2)
			{
				fprintf(stderr, "Failure while reading file %s\n", argv[1]);
			}
		}
		address_end = address_off + filesize;
		fclose(f);
		for(cur = filebuf; cur - filebuf < (filesize/2); ++cur)
		{
			*cur = (*cur >> 8) | (*cur << 8);
		}
		if (!address_off) {
			uint32_t start = filebuf[2] << 16 | filebuf[3];
			uint32_t int_2 = filebuf[0x68/2] << 16 | filebuf[0x6A/2];
			uint32_t int_4 = filebuf[0x70/2] << 16 | filebuf[0x72/2];
			uint32_t int_6 = filebuf[0x78/2] << 16 | filebuf[0x7A/2];
			named_labels = add_label(named_labels, "start", start);
			named_labels = add_label(named_labels, "int_2", int_2);
			named_labels = add_label(named_labels, "int_4", int_4);
			named_labels = add_label(named_labels, "int_6", int_6);
			if (!def || !only) {
				def = defer(start, def);
				def = defer(int_2, def);
				def = defer(int_4, def);
				def = defer(int_6, def);
			}
			named_labels = weak_label(named_labels, "illegal_inst", filebuf[0x10/2] << 16 | filebuf[0x12/2]);
			named_labels = weak_label(named_labels, "div_zero", filebuf[0x14/2] << 16 | filebuf[0x16/2]);
			named_labels = weak_label(named_labels, "chk_exception", filebuf[0x18/2] << 16 | filebuf[0x1A/2]);
			named_labels = weak_label(named_labels, "trapv", filebuf[0x1C/2] << 16 | filebuf[0x1E/2]);
			named_labels = weak_label(named_labels, "line_a_trap", filebuf[0x28/2] << 16 | filebuf[0x2A/2]);
			named_labels = weak_label(named_labels, "line_f_trap", filebuf[0x2C/2] << 16 | filebuf[0x2E/2]);
		}
	}
	if (do_cd_labels) {
		if (main_cpu) {
			named_labels = weak_label(named_labels, "_bios_reset", 0x280);
			named_labels = weak_label(named_labels, "_bios_entry", 0x284);
			named_labels = weak_label(named_labels, "_bios_init", 0x288);
			named_labels = weak_label(named_labels, "_bios_init_sp", 0x28C);
			named_labels = weak_label(named_labels, "_bios_vint", 0x290);
			named_labels = weak_label(named_labels, "_bios_set_hint", 0x294);
			named_labels = weak_label(named_labels, "_bios_poll_io", 0x298);
			named_labels = weak_label(named_labels, "_bios_detect_io", 0x29C);
			named_labels = weak_label(named_labels, "_bios_clear_vram", 0x2A0);
			named_labels = weak_label(named_labels, "_bios_clear_nmtbl", 0x2A4);
			named_labels = weak_label(named_labels, "_bios_clear_vsram", 0x2A8);
			named_labels = weak_label(named_labels, "_bios_init_vdp", 0x2AC);
			named_labels = weak_label(named_labels, "_bios_vdp_loadregs", 0x2B0);
			named_labels = weak_label(named_labels, "_bios_vdp_fill", 0x2B4);
			named_labels = weak_label(named_labels, "_bios_clear_vram_range", 0x2B8);
			named_labels = weak_label(named_labels, "_bios_clear_vram_range_dma", 0x2BC);
			named_labels = weak_label(named_labels, "_bios_vram_dma_fill", 0x2C0);
			named_labels = weak_label(named_labels, "_bios_update_nmtbl", 0x2C4);
			named_labels = weak_label(named_labels, "_bios_update_nmtbl_template", 0x2C8);
			named_labels = weak_label(named_labels, "_bios_fill_nmtbl", 0x2CC);
			named_labels = weak_label(named_labels, "_bios_vdp_dma", 0x2D0);
			named_labels = weak_label(named_labels, "_bios_vdp_dma_wordram", 0x2D4);
			named_labels = weak_label(named_labels, "_bios_vdp_display_enable", 0x2D8);
			named_labels = weak_label(named_labels, "_bios_vdp_display_disable", 0x2DC);
			named_labels = weak_label(named_labels, "_bios_pal_buffer", 0x2E0);
			named_labels = weak_label(named_labels, "_bios_pal_buffer_update", 0x2E4);
			named_labels = weak_label(named_labels, "_bios_pal_dma", 0x2E8);
			named_labels = weak_label(named_labels, "_bios_gfx_decomp", 0x2EC);
			named_labels = weak_label(named_labels, "_bios_gfx_decomp_ram", 0x2F0);
			named_labels = weak_label(named_labels, "_bios_update_sprites", 0x2F4);
			named_labels = weak_label(named_labels, "_bios_clear_ram", 0x2F8);
			named_labels = weak_label(named_labels, "_bios_display_sprite", 0x300);
			named_labels = weak_label(named_labels, "_bios_wait_vint", 0x304);
			named_labels = weak_label(named_labels, "_bios_wait_vint_flags", 0x308);
			named_labels = weak_label(named_labels, "_bios_dma_sat", 0x30C);
			named_labels = weak_label(named_labels, "_bios_set_hint_direct", 0x314);
			named_labels = weak_label(named_labels, "_bios_disable_hint", 0x318);
			named_labels = weak_label(named_labels, "_bios_print", 0x31C);
			named_labels = weak_label(named_labels, "_bios_load_user_font", 0x320);
			named_labels = weak_label(named_labels, "_bios_load_bios_font", 0x324);
			named_labels = weak_label(named_labels, "_bios_load_bios_font_default", 0x328);
			//TODO: more functions in the middle here
			named_labels = weak_label(named_labels, "_bios_prng_mod", 0x338);
			named_labels = weak_label(named_labels, "_bios_prng", 0x33C);
			named_labels = weak_label(named_labels, "_bios_clear_comm", 0x340);
			named_labels = weak_label(named_labels, "_bios_comm_update", 0x344);
			//TODO: more functions in the middle here
			named_labels = weak_label(named_labels, "_bios_sega_logo", 0x364);
			named_labels = weak_label(named_labels, "_bios_set_vint", 0x368);
			//TODO: more functions at the end here

			named_labels = weak_label(named_labels, "WORD_RAM", 0x200000);
			named_labels = weak_label(named_labels, "CD_RESET_IFL2", 0xA12000);
			named_labels = weak_label(named_labels, "CD_RESET_IFL2_BYTE", 0xA12001);
			named_labels = weak_label(named_labels, "CD_WRITE_PROTECT", 0xA12002);
			named_labels = weak_label(named_labels, "CD_MEM_MODE", 0xA12003);
			named_labels = weak_label(named_labels, "CDC_CTRL", 0xA12004);
			named_labels = weak_label(named_labels, "HINT_VECTOR", 0xA12006);
			named_labels = weak_label(named_labels, "CDC_HOST_DATA", 0xA12008);
			named_labels = weak_label(named_labels, "STOP_WATCH", 0xA1200C);
			named_labels = weak_label(named_labels, "COMM_MAIN_FLAG", 0xA1200E);
			named_labels = weak_label(named_labels, "COMM_SUB_FLAG", 0xA1200F);
			named_labels = weak_label(named_labels, "COMM_CMD0", 0xA12010);
			named_labels = weak_label(named_labels, "COMM_CMD1", 0xA12012);
			named_labels = weak_label(named_labels, "COMM_CMD2", 0xA12014);
			named_labels = weak_label(named_labels, "COMM_CMD3", 0xA12016);
			named_labels = weak_label(named_labels, "COMM_CMD4", 0xA12018);
			named_labels = weak_label(named_labels, "COMM_CMD5", 0xA1201A);
			named_labels = weak_label(named_labels, "COMM_CMD6", 0xA1201C);
			named_labels = weak_label(named_labels, "COMM_CMD7", 0xA1201E);
			named_labels = weak_label(named_labels, "COMM_STATUS0", 0xA12020);
			named_labels = weak_label(named_labels, "COMM_STATUS1", 0xA12022);
			named_labels = weak_label(named_labels, "COMM_STATUS2", 0xA12024);
			named_labels = weak_label(named_labels, "COMM_STATUS3", 0xA12026);
			named_labels = weak_label(named_labels, "COMM_STATUS4", 0xA12028);
			named_labels = weak_label(named_labels, "COMM_STATUS5", 0xA1202A);
			named_labels = weak_label(named_labels, "COMM_STATUS6", 0xA1202C);
			named_labels = weak_label(named_labels, "COMM_STATUS7", 0xA1202E);
		} else {
			named_labels = weak_label(named_labels, "bios_common_work", 0x5E80);
			named_labels = weak_label(named_labels, "_setjmptbl", 0x5F0A);
			named_labels = weak_label(named_labels, "_waitvsync", 0x5F10);
			named_labels = weak_label(named_labels, "_buram", 0x5F16);
			named_labels = weak_label(named_labels, "_cdboot", 0x5F1C);
			named_labels = weak_label(named_labels, "_cdbios", 0x5F22);
			named_labels = weak_label(named_labels, "_usercall0", 0x5F28);
			named_labels = weak_label(named_labels, "_usercall1", 0x5F2E);
			named_labels = weak_label(named_labels, "_usercall2", 0x5F34);
			named_labels = weak_label(named_labels, "_usercall2Address", 0x5F36);
			named_labels = weak_label(named_labels, "_usercall3", 0x5F3A);
			named_labels = weak_label(named_labels, "_adrerr", 0x5F40);
			named_labels = weak_label(named_labels, "_adrerrAddress", 0x5F42);
			named_labels = weak_label(named_labels, "_coderr", 0x5F46);
			named_labels = weak_label(named_labels, "_coderrAddress", 0x5F48);
			named_labels = weak_label(named_labels, "_diverr", 0x5F4C);
			named_labels = weak_label(named_labels, "_diverrAddress", 0x5F4E);
			named_labels = weak_label(named_labels, "_chkerr", 0x5F52);
			named_labels = weak_label(named_labels, "_chkerrAddress", 0x5F54);
			named_labels = weak_label(named_labels, "_trperr", 0x5F58);
			named_labels = weak_label(named_labels, "_trperrAddress", 0x5F5A);
			named_labels = weak_label(named_labels, "_spverr", 0x5F5E);
			named_labels = weak_label(named_labels, "_spverrAddress", 0x5F60);
			named_labels = weak_label(named_labels, "_trace", 0x5F64);
			named_labels = weak_label(named_labels, "_traceAddress", 0x5F66);
			named_labels = weak_label(named_labels, "_nocod0", 0x5F6A);
			named_labels = weak_label(named_labels, "_nocod0Address", 0x5F6C);
			named_labels = weak_label(named_labels, "_nocod0", 0x5F70);
			named_labels = weak_label(named_labels, "_nocod0Address", 0x5F72);
			named_labels = weak_label(named_labels, "_slevel1", 0x5F76);
			named_labels = weak_label(named_labels, "_slevel1Address", 0x5F78);
			named_labels = weak_label(named_labels, "_slevel2", 0x5F7C);
			named_labels = weak_label(named_labels, "_slevel2Address", 0x5F7E);
			named_labels = weak_label(named_labels, "_slevel3", 0x5F82);
			named_labels = weak_label(named_labels, "_slevel3Address", 0x5F84);
			named_labels = weak_label(named_labels, "WORD_RAM_2M", 0x80000);
			named_labels = weak_label(named_labels, "WORD_RAM_1M", 0xC0000);
			named_labels = weak_label(named_labels, "LED_CONTROL", 0xFFFF8000);
			named_labels = weak_label(named_labels, "VERSION_RESET", 0xFFFF8001);
			named_labels = weak_label(named_labels, "MEM_MODE_WORD", 0xFFFF8002);
			named_labels = weak_label(named_labels, "MEM_MODE_BYTE", 0xFFFF8003);
			named_labels = weak_label(named_labels, "CDC_CTRL", 0xFFFF8004);
			named_labels = weak_label(named_labels, "CDC_AR", 0xFFFF8005);
			named_labels = weak_label(named_labels, "CDC_REG_DATA_WORD", 0xFFFF8006);
			named_labels = weak_label(named_labels, "CDC_REG_DATA", 0xFFFF8007);
			named_labels = weak_label(named_labels, "CDC_HOST_DATA", 0xFFFF8008);
			named_labels = weak_label(named_labels, "CDC_DMA_ADDR", 0xFFFF800A);
			named_labels = weak_label(named_labels, "STOP_WATCH", 0xFFFF800C);
			named_labels = weak_label(named_labels, "COMM_MAIN_FLAG", 0xFFFF800E);
			named_labels = weak_label(named_labels, "COMM_SUB_FLAG", 0xFFFF800F);
			named_labels = weak_label(named_labels, "COMM_CMD0", 0xFFFF8010);
			named_labels = weak_label(named_labels, "COMM_CMD1", 0xFFFF8012);
			named_labels = weak_label(named_labels, "COMM_CMD2", 0xFFFF8014);
			named_labels = weak_label(named_labels, "COMM_CMD3", 0xFFFF8016);
			named_labels = weak_label(named_labels, "COMM_CMD4", 0xFFFF8018);
			named_labels = weak_label(named_labels, "COMM_CMD5", 0xFFFF801A);
			named_labels = weak_label(named_labels, "COMM_CMD6", 0xFFFF801C);
			named_labels = weak_label(named_labels, "COMM_CMD7", 0xFFFF801E);
			named_labels = weak_label(named_labels, "COMM_STATUS0", 0xFFFF8020);
			named_labels = weak_label(named_labels, "COMM_STATUS1", 0xFFFF8022);
			named_labels = weak_label(named_labels, "COMM_STATUS2", 0xFFFF8024);
			named_labels = weak_label(named_labels, "COMM_STATUS3", 0xFFFF8026);
			named_labels = weak_label(named_labels, "COMM_STATUS4", 0xFFFF8028);
			named_labels = weak_label(named_labels, "COMM_STATUS5", 0xFFFF802A);
			named_labels = weak_label(named_labels, "COMM_STATUS6", 0xFFFF802C);
			named_labels = weak_label(named_labels, "COMM_STATUS7", 0xFFFF802E);
			named_labels = weak_label(named_labels, "TIMER_WORD", 0xFFFF8030);
			named_labels = weak_label(named_labels, "TIMER", 0xFFFF8031);
			named_labels = weak_label(named_labels, "INT_MASK_WORD", 0xFFFF8032);
			named_labels = weak_label(named_labels, "INT_MASK", 0xFFFF8033);
			named_labels = weak_label(named_labels, "CDD_FADER", 0xFFFF8034);
			named_labels = weak_label(named_labels, "CDD_CTRL_WORD", 0xFFFF8036);
			named_labels = weak_label(named_labels, "CDD_CTRL_BYTE", 0xFFFF8037);
		}
	}
	uint32_t size, tmp_addr;
	uint32_t address;
	rom_def rom = {
		.address_off = address_off,
		.address_end = address_end,
		.buffer = filebuf
	};
	uint8_t valid_address;
	while(def) {
		do {
			valid_address = 0;
			address = def->address;
			if (!is_visited(address)) {
				address &= 0xFFFFFF;
				if (address < address_end && address >= address_off) {
					valid_address = 1;
				}
			}
			tmpd = def;
			def = def->next;
			free(tmpd);
		} while(def && !valid_address);
		if (!valid_address) {
			break;
		}
		for(;;) {
			if ((address & 0xFFFFFF) > address_end || address < address_off) {
				break;
			}
			visit(address);
			address = m68k_decode(fetch, &rom, &instbuf, address);
			//m68k_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", instbuf.address, disbuf);
			check_reference(&instbuf, &(instbuf.src));
			check_reference(&instbuf, &(instbuf.dst));
			if (instbuf.op == M68K_ILLEGAL || instbuf.op == M68K_RTS || instbuf.op == M68K_RTE || instbuf.op == M68K_INVALID) {
				break;
			}
			if (instbuf.op == M68K_BCC || instbuf.op == M68K_DBCC || instbuf.op == M68K_BSR) {
				if (instbuf.op == M68K_BCC && instbuf.extra.cond == COND_TRUE) {
					address = instbuf.address + 2 + instbuf.src.params.immed;
					reference(address);
					if (is_visited(address)) {
						break;
					}
				} else {
					tmp_addr = instbuf.address + 2 + instbuf.src.params.immed;
					reference(tmp_addr);
					def = defer(tmp_addr, def);
				}
			} else if(instbuf.op == M68K_JMP) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					address = instbuf.src.params.immed;
					if (is_visited(address)) {
						break;
					}
				} else if (instbuf.src.addr_mode == MODE_PC_DISPLACE) {
					address = instbuf.src.params.regs.displacement + instbuf.address + 2;
					if (is_visited(address)) {
						break;
					}
				} else {
					break;
				}
			} else if(instbuf.op == M68K_JSR) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					def = defer(instbuf.src.params.immed, def);
				} else if (instbuf.src.addr_mode == MODE_PC_DISPLACE) {
					def = defer(instbuf.src.params.regs.displacement + instbuf.address + 2, def);
				}
			}
		}
	}
	if (labels) {
		for (address = 0; address < address_off; address++) {
			if (is_label(address)) {
				char key[MAX_INT_KEY_SIZE];
				tern_int_key(address, key);
				label_names *names = tern_find_ptr(named_labels, key);
				if (names) {
					for (int i = 0; i < names->num_labels; i++)
					{
						printf("%s equ $%X\n", names->labels[i], address);
					}
				} else  {
					printf("ADR_%X equ $%X\n", address, address);
				}
			}
		}
		for (address = address_end; address < (16*1024*1024); address++) {
			if (is_label(address)) {
				char key[MAX_INT_KEY_SIZE];
				tern_int_key(address, key);
				label_names *names = tern_find_ptr(named_labels, key);
				if (names) {
					for (int i = 0; i < names->num_labels; i++)
					{
						printf("%s equ $%X\n", names->labels[i], names->full_address);
					}
				} else  {
					printf("ADR_%X equ $%X\n", address, address);
				}
			}
		}
		puts("");
	}
	for (address = address_off; address < address_end; address+=2) {
		if (is_visited(address)) {
			m68k_decode(fetch, &rom, &instbuf, address);
			if (labels) {
				m68k_disasm_labels(&instbuf, disbuf, label_fun, named_labels);
				char keybuf[MAX_INT_KEY_SIZE];
				label_names * names = tern_find_ptr(named_labels, tern_int_key(address, keybuf));
				if (names)
				{
					for (int i = 0; i < names->num_labels; i++)
					{
						printf("%s:\n", names->labels[i]);
					}
				} else if (is_label(instbuf.address)) {
					printf("ADR_%X:\n", instbuf.address);
				}
				if (addr) {
					printf("\t%s\t;%X\n", disbuf, instbuf.address);
				} else {
					printf("\t%s\n", disbuf);
				}
			} else {
				m68k_disasm(&instbuf, disbuf);
				printf("%X: %s\n", instbuf.address, disbuf);
			}
		}
	}
	return 0;
}
