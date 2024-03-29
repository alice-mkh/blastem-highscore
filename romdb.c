#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"
#include "hash.h"
#include "genesis.h"
#include "menu.h"
#include "xband.h"
#include "realtec.h"
#include "nor.h"
#include "sega_mapper.h"
#include "multi_game.h"
#include "megawifi.h"
#include "jcart.h"
#include "blastem.h"
#include "sft_mapper.h"

#define DOM_TITLE_START 0x120
#define DOM_TITLE_END 0x150
#define TITLE_START DOM_TITLE_END
#define TITLE_END (TITLE_START+48)
#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define REGION_START 0x1F0

char const *save_type_name(uint8_t save_type)
{
	if (save_type == SAVE_I2C) {
		return "EEPROM";
	} else if(save_type == SAVE_NOR) {
		return "NOR Flash";
	} else if(save_type == SAVE_HBPT) {
		return "Heartbeat Personal Trainer";
	}
	return "SRAM";
}

tern_node *get_rom_db()
{
	static tern_node *db;
	if (!db) {
		db = parse_bundled_config("rom.db");
		if (!db) {
			fatal_error("Failed to load ROM DB\n");
		}
	}
	return db;
}

void free_rom_info(rom_info *info)
{
	free(info->name);
	if (info->save_type != SAVE_NONE) {
		free(info->save_buffer);
		if (info->save_type == SAVE_I2C) {
			free(info->eeprom_map);
		} else if (info->save_type == SAVE_NOR) {
			free(info->nor);
		}
	}
	free(info->map);
}

void cart_serialize(system_header *sys, serialize_buffer *buf)
{
	if (sys->type != SYSTEM_GENESIS) {
		return;
	}
	genesis_context *gen = (genesis_context *)sys;
	if (gen->mapper_type == MAPPER_NONE) {
		return;
	}
	start_section(buf, SECTION_MAPPER);
	save_int8(buf, gen->mapper_type);
	switch(gen->mapper_type)
	{
	case MAPPER_SEGA:
	case MAPPER_SEGA_SRAM:
	case MAPPER_SEGA_MED_V2:
		sega_mapper_serialize(gen, buf);
		break;
	case MAPPER_REALTEC:
		realtec_serialize(gen, buf);
		break;
	case MAPPER_XBAND:
		xband_serialize(gen, buf);
		break;
	case MAPPER_MULTI_GAME:
		multi_game_serialize(gen, buf);
		break;
	}
	end_section(buf);
}

void cart_deserialize(deserialize_buffer *buf, void *vcontext)
{
	genesis_context *gen = vcontext;
	uint8_t mapper_type = load_int8(buf);
	if (mapper_type != gen->mapper_type && (mapper_type != MAPPER_SEGA || gen->mapper_type != MAPPER_SEGA_SRAM)) {
		warning("Mapper type mismatch, skipping load of mapper state\n");
		return;
	}
	switch(gen->mapper_type)
	{
	case MAPPER_SEGA:
	case MAPPER_SEGA_SRAM:
		sega_mapper_deserialize(buf, gen);
		break;
	case MAPPER_REALTEC:
		realtec_deserialize(buf, gen);
		break;
	case MAPPER_XBAND:
		xband_deserialize(buf, gen);
		break;
	case MAPPER_MULTI_GAME:
		multi_game_deserialize(buf, gen);
		break;
	}
}

char *get_header_name(uint8_t *rom)
{
	//TODO: Should probably prefer the title field that corresponds to the user's region preference
	uint8_t *last = rom + TITLE_END - 1;
	uint8_t *src = rom + TITLE_START;

	for (;;)
	{
		while (last > src && (*last <=  0x20 || *last >= 0x80))
		{
			last--;
		}
		if (last == src) {
			if (src == rom + TITLE_START) {
				src = rom + DOM_TITLE_START;
				last = rom + DOM_TITLE_END - 1;
			} else {
				return strdup("UNKNOWN");
			}
		} else {
			last++;
			char *ret = malloc(last - src + 1);
			uint8_t *dst;
			uint8_t last_was_space = 1;
			for (dst = ret; src < last; src++)
			{
				if (*src >= 0x20 && *src < 0x80) {
					if (*src == ' ') {
						if (last_was_space) {
							continue;
						}
						last_was_space = 1;
					} else {
						last_was_space = 0;
					}
					*(dst++) = *src;
				}
			}
			*dst = 0;
			return ret;
		}
	}
}

char *region_chars = "JUEW";
uint8_t region_bits[] = {REGION_J, REGION_U, REGION_E, REGION_J|REGION_U|REGION_E};

uint8_t translate_region_char(uint8_t c)
{
	for (int i = 0; i < sizeof(region_bits); i++)
	{
		if (c == region_chars[i]) {
			return region_bits[i];
		}
	}
	uint8_t bin_region = 0;
	if (c >= '0' && c <= '9') {
		bin_region = c - '0';
	} else if (c >= 'A' && c <= 'F') {
		bin_region = c - 'A' + 0xA;
	} else if (c >= 'a' && c <= 'f') {
		bin_region = c - 'a' + 0xA;
	}
	uint8_t ret = 0;
	if (bin_region & 8) {
		ret |= REGION_E;
	}
	if (bin_region & 4) {
		ret |= REGION_U;
	}
	if (bin_region & 1) {
		ret |= REGION_J;
	}
	return ret;
}

uint8_t get_header_regions(uint8_t *rom)
{
	uint8_t regions = 0;
	for (int i = 0; i < 3; i++)
	{
		regions |= translate_region_char(rom[REGION_START + i]);
	}
	return regions;
}

uint32_t get_u32be(uint8_t *data)
{
	return *data << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

uint32_t calc_mask(uint32_t src_size, uint32_t start, uint32_t end)
{
	uint32_t map_size = end-start+1;
	if (src_size < map_size) {
		return nearest_pow2(src_size)-1;
	} else if (!start) {
		return 0xFFFFFF;
	} else {
		return nearest_pow2(map_size)-1;
	}
}

uint8_t has_ram_header(uint8_t *rom, uint32_t rom_size)
{
	return rom_size >= (RAM_END + 4) && rom[RAM_ID] == 'R' && rom[RAM_ID + 1] == 'A';
}

uint32_t read_ram_header(rom_info *info, uint8_t *rom)
{
	uint32_t ram_start = get_u32be(rom + RAM_START);
	uint32_t ram_end = get_u32be(rom + RAM_END);
	uint32_t ram_flags = info->save_type = rom[RAM_FLAGS] & RAM_FLAG_MASK;
	ram_start &= 0xFFFFFE;
	ram_end |= 1;
	if (ram_start >= 0x800000) {
		info->save_buffer = NULL;
		return ram_start;
	}
	info->save_mask = ram_end - ram_start;
	uint32_t save_size = info->save_mask + 1;
	if (ram_flags != RAM_FLAG_BOTH) {
		save_size /= 2;
	}
	info->save_size = save_size;
	info->save_buffer = calloc(save_size, 1);
	return ram_start;
}

void add_memmap_header(rom_info *info, uint8_t *rom, uint32_t size, memmap_chunk const *base_map, int base_chunks)
{
	uint32_t rom_end = get_u32be(rom + ROM_END) + 1;
	uint32_t rom_end_raw = rom_end;
	if (size > rom_end) {
		rom_end = size;
	} else if (rom_end > nearest_pow2(size)) {
		rom_end = nearest_pow2(size);
	}
	info->save_type = SAVE_NONE;
	uint8_t is_med_ssf = size >= 0x108 && !memcmp("SEGA SSF", rom + 0x100, 8);
	if (is_med_ssf || (size > 0x400000 && rom_end_raw <= 0x400000)) {
		if (is_med_ssf && rom_end < 16*1024*1024) {
			info->rom = rom = realloc(rom, 16*1024*1024);
		}
		info->mapper_start_index = 0;
		info->mapper_type = is_med_ssf ? MAPPER_SEGA_MED_V2 : MAPPER_SEGA;
		info->map_chunks = base_chunks + 9;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*9);
		memcpy(info->map+9, base_map, sizeof(memmap_chunk) * base_chunks);

		int i;
		uint16_t map_flags;
		if (is_med_ssf) {
			i = 0;
			map_flags = info->map[i].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
			info->save_type = RAM_FLAG_BOTH;
			info->save_size = 256*1024;
			info->save_mask = info->save_size - 1;
			info->save_buffer = rom + 16*1024*1024 - 256*1024;
		} else {
			i = 1;
			map_flags = info->map[i].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE | MMAP_FUNC_NULL;
			info->map[0].start = 0;
			info->map[0].end = 0x80000;
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;

			if (has_ram_header(rom, size)){
				read_ram_header(info, rom);
			}
		}
		static const write_8_fun med_w8[] = {
			write_med_ram0_b,
			write_med_ram1_b,
			write_med_ram2_b,
			write_med_ram3_b,
			write_med_ram4_b,
			write_med_ram5_b,
			write_med_ram6_b,
			write_med_ram7_b,
		};
		static const write_16_fun med_w16[] = {
			write_med_ram0_w,
			write_med_ram1_w,
			write_med_ram2_w,
			write_med_ram3_w,
			write_med_ram4_w,
			write_med_ram5_w,
			write_med_ram6_w,
			write_med_ram7_w,
		};

		for (; i < 8; i++)
		{
			info->map[i].start = i * 0x80000;
			info->map[i].end = (i + 1) * 0x80000;
			info->map[i].mask = 0x7FFFF;
			info->map[i].buffer = (i + 1) * 0x80000 <= size ? rom + i * 0x80000 : rom;
			info->map[i].ptr_index = i;
			info->map[i].flags = map_flags;

			info->map[i].read_16 = is_med_ssf ? NULL : (read_16_fun)read_sram_w;//these will only be called when mem_pointers[i] == NULL
			info->map[i].read_8 = is_med_ssf ? NULL : (read_8_fun)read_sram_b;
			if (is_med_ssf) {
				info->map[i].write_16 = med_w16[i];
				info->map[i].write_8 = med_w8[i];
			} else {
				info->map[i].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
				info->map[i].write_8 = (write_8_fun)write_sram_area_b;
			}
		}
		info->map[8].start = 0xA13000;
		info->map[8].end = 0xA13100;
		info->map[8].mask = 0xFF;
		info->map[8].write_16 = (write_16_fun)write_bank_reg_w;
		info->map[8].write_8 = (write_8_fun)write_bank_reg_b;
		if (is_med_ssf) {
			info->map[8].read_16 = med_reg_read_w;
			info->map[8].read_8 = med_reg_read_b;
		}
		return;
	} else if(!memcmp("SEGA MEGAWIFI", rom + 0x100, strlen("SEGA MEGAWIFI"))) {
		info->mapper_type = MAPPER_NONE;
		info->map_chunks = base_chunks + 2;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*2);
		memcpy(info->map+2, base_map, sizeof(memmap_chunk) * base_chunks);
		info->save_size = 0x400000;
		info->save_bus = RAM_FLAG_BOTH;
		info->save_type = SAVE_NOR;
		info->map[0].start = 0;
		info->map[0].end = 0x400000;
		info->map[0].mask = 0xFFFFFF;
		info->map[0].write_16 = nor_flash_write_w;
		info->map[0].write_8 = nor_flash_write_b;
		info->map[0].read_16 = nor_flash_read_w;
		info->map[0].read_8 = nor_flash_read_b;
		info->map[0].flags = MMAP_READ_CODE | MMAP_CODE;
		info->map[0].buffer = info->save_buffer = calloc(info->save_size, 1);
		uint32_t init_size = size < info->save_size ? size : info->save_size;
		memcpy(info->save_buffer, rom, init_size);
		byteswap_rom(info->save_size, (uint16_t *)info->save_buffer);
		info->nor = calloc(1, sizeof(nor_state));
		nor_flash_init(info->nor, info->save_buffer, info->save_size, 128, 0xDA45, RAM_FLAG_BOTH);
		info->nor->cmd_address1 = 0xAAB;
		info->nor->cmd_address2 = 0x555;
		info->map[1].start = 0xA130C0;
		info->map[1].end = 0xA130D0;
		info->map[1].mask = 0xFFFFFF;
		if (!strcmp(
			"on",
			tern_find_path_default(config, "system\0megawifi\0", (tern_val){.ptrval="off"}, TVAL_PTR).ptrval)
		) {
			info->map[1].write_16 = megawifi_write_w;
			info->map[1].write_8 = megawifi_write_b;
			info->map[1].read_16 = megawifi_read_w;
			info->map[1].read_8 = megawifi_read_b;
		} else {
			warning("ROM uses MegaWiFi, but it is disabled\n");
		}
		return;
	} else if (has_ram_header(rom, size)) {
		uint32_t ram_start = read_ram_header(info, rom);

		if (info->save_buffer) {
			info->map_chunks = base_chunks + (ram_start >= rom_end ? 2 : 3);
			info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
			memset(info->map, 0, sizeof(memmap_chunk)*2);
			memcpy(info->map+2, base_map, sizeof(memmap_chunk) * base_chunks);

			if (ram_start >= rom_end) {
				info->map[0].end = rom_end < 0x400000 ? nearest_pow2(rom_end) - 1 : 0xFFFFFF;
				if (info->map[0].end > ram_start) {
					info->map[0].end = ram_start;
				}
				//TODO: ROM mirroring
				info->map[0].mask = 0xFFFFFF;
				info->map[0].flags = MMAP_READ;
				info->map[0].buffer = rom;

				info->map[1].start = ram_start;
				info->map[1].mask = info->save_mask;
				info->map[1].end = ram_start + info->save_mask + 1;
				info->map[1].flags = MMAP_READ | MMAP_WRITE;

				if (info->save_type == RAM_FLAG_ODD) {
					info->map[1].flags |= MMAP_ONLY_ODD;
				} else if (info->save_type == RAM_FLAG_EVEN) {
					info->map[1].flags |= MMAP_ONLY_EVEN;
				} else {
					info->map[1].flags |= MMAP_CODE;
				}
				info->map[1].buffer = info->save_buffer;
			} else {
				//Assume the standard Sega mapper
				info->mapper_type = MAPPER_SEGA_SRAM;
				info->map[0].end = 0x200000;
				info->map[0].mask = 0xFFFFFF;
				info->map[0].flags = MMAP_READ;
				info->map[0].buffer = rom;

				info->map[1].start = 0x200000;
				info->map[1].end = 0x400000;
				info->map[1].mask = 0x1FFFFF;
				info->map[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
				info->map[1].ptr_index = info->mapper_start_index = 0;
				info->map[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
				info->map[1].read_8 = (read_8_fun)read_sram_b;
				info->map[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
				info->map[1].write_8 = (write_8_fun)write_sram_area_b;
				info->map[1].buffer = rom + 0x200000;

				//Last entry in the base map is a catch all one that needs to be
				//after all the other entries
				memmap_chunk *unused = info->map + info->map_chunks - 2;
				memmap_chunk *last = info->map + info->map_chunks - 1;
				*last = *unused;
				last = unused;
				memset(last, 0, sizeof(memmap_chunk));
				last->start = 0xA13000;
				last->end = 0xA13100;
				last->mask = 0xFF;
				last->write_16 = (write_16_fun)write_bank_reg_w;
				last->write_8 = (write_8_fun)write_bank_reg_b;
			}
			return;
		}
	}

	info->map_chunks = base_chunks + 1;
	info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
	memset(info->map, 0, sizeof(memmap_chunk));
	memcpy(info->map+1, base_map, sizeof(memmap_chunk) * base_chunks);

	info->map[0].end = rom_end > 0x400000 ? rom_end : 0x400000;
	info->map[0].mask = rom_end < 0x400000 ? nearest_pow2(rom_end) - 1 : 0xFFFFFF;
	info->map[0].flags = MMAP_READ;
	info->map[0].buffer = rom;
	info->save_type = SAVE_NONE;
}

rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	info.mapper_type = MAPPER_NONE;
	info.mapper_start_index = 0;
	info.name = get_header_name(rom);
	info.regions = get_header_regions(rom);
	info.is_save_lock_on = 0;
	info.rom = rom;
	info.rom_size = rom_size;
	info.wants_cd = 0;
	for (uint32_t offset = 0x190; offset < rom_size && offset < 0x1A0; offset++)
	{
		uint8_t invalid = 0;
		switch(rom[offset])
		{
		case 'J':
		case '6':
		case '0':
		case 'A':
		case '4':
		case 'G':
		case 'L':
		case 'M':
		case 'B':
		case 'K':
		case 'R':
		case 'D':
		case 'P':
			//valid device letter
			//TODO: do something with these
			break;
		case ' ':
			//blanks are okay
			break;
		case 'F':
		case 'T':
			//unreleased peripheral, probably garbage
			invalid = 1;
			break;
		case 'C':
			info.wants_cd = 1;
			break;
		default:
			invalid = 1;
		}
		if (invalid) {
			info.wants_cd = 0;
			break;
		}
	}
	add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	info.port1_override = info.port2_override = info.ext_override = info.mouse_mode = NULL;

	return info;
}

typedef struct {
	rom_info     *info;
	uint8_t      *rom;
	uint8_t      *lock_on;
	tern_node    *root;
	tern_node    *rom_db;
	uint32_t     rom_size;
	uint32_t     lock_on_size;
	int          index;
	int          num_els;
	uint16_t     ptr_index;
} map_iter_state;

void eeprom_read_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	if (valtype != TVAL_PTR) {
		fprintf(stderr, "bit %s has a non-scalar value", key);
		return;
	}
	char *pin = val.ptrval;
	if (strcmp(pin, "sda")) {
		fprintf(stderr, "bit %s is connected to unrecognized read pin %s", key, pin);
		return;
	}
	eeprom_map *map = data;
	map->sda_read_bit = bit;
}

void eeprom_write_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	if (valtype != TVAL_PTR) {
		fprintf(stderr, "bit %s has a non-scalar value", key);
		return;
	}
	char *pin = val.ptrval;
	eeprom_map *map = data;
	if (!strcmp(pin, "sda")) {
		map->sda_write_mask = 1 << bit;
		return;
	}
	if (!strcmp(pin, "scl")) {
		map->scl_mask = 1 << bit;
		return;
	}
	fprintf(stderr, "bit %s is connected to unrecognized write pin %s", key, pin);
}

void process_sram_def(char *key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char * size = tern_find_path(state->root, "SRAM\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type SRAM, but the SRAM size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("SRAM size %s is invalid\n", size);
		}
		state->info->save_mask = nearest_pow2(state->info->save_size)-1;
		state->info->save_buffer = calloc(state->info->save_size, 1);
		char *bus = tern_find_path(state->root, "SRAM\0bus\0", TVAL_PTR).ptrval;
		if (!strcmp(bus, "odd")) {
			state->info->save_type = RAM_FLAG_ODD;
		} else if(!strcmp(bus, "even")) {
			state->info->save_type = RAM_FLAG_EVEN;
		} else {
			state->info->save_type = RAM_FLAG_BOTH;
		}
	}
}

void process_eeprom_def(char * key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char * size = tern_find_path(state->root, "EEPROM\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type EEPROM, but the EEPROM size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("EEPROM size %s is invalid\n", size);
		}
		char *etype = tern_find_path(state->root, "EEPROM\0type\0", TVAL_PTR).ptrval;
		if (!etype) {
			etype = "i2c";
		}
		if (!strcmp(etype, "i2c")) {
			state->info->save_type = SAVE_I2C;
		} else {
			fatal_error("EEPROM type %s is invalid\n", etype);
		}
		state->info->save_buffer = malloc(state->info->save_size);
		memset(state->info->save_buffer, 0xFF, state->info->save_size);
		state->info->eeprom_map = malloc(sizeof(eeprom_map) * state->num_els);
		memset(state->info->eeprom_map, 0, sizeof(eeprom_map) * state->num_els);
	}
}

void process_nor_def(char *key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char *size = tern_find_path(state->root, "NOR\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("NOR size %s is invalid\n", size);
		}
		char *page_size = tern_find_path(state->root, "NOR\0page_size\0", TVAL_PTR).ptrval;
		if (!page_size) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR page size is not defined\n", state->index, key);
		}
		uint32_t save_page_size = atoi(page_size);
		if (!save_page_size) {
			fatal_error("NOR page size %s is invalid\n", page_size);
		}
		char *product_id = tern_find_path(state->root, "NOR\0product_id\0", TVAL_PTR).ptrval;
		if (!product_id) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR product ID is not defined\n", state->index, key);
		}
		uint16_t save_product_id = strtol(product_id, NULL, 16);
		char *bus = tern_find_path(state->root, "NOR\0bus\0", TVAL_PTR).ptrval;
		if (!strcmp(bus, "odd")) {
			state->info->save_bus = RAM_FLAG_ODD;
		} else if(!strcmp(bus, "even")) {
			state->info->save_bus = RAM_FLAG_EVEN;
		} else {
			state->info->save_bus = RAM_FLAG_BOTH;
		}
		state->info->save_type = SAVE_NOR;
		state->info->save_buffer = malloc(state->info->save_size);
		char *init = tern_find_path_default(state->root, "NOR\0init\0", (tern_val){.ptrval="FF"}, TVAL_PTR).ptrval;
		if (!strcmp(init, "ROM")) {
			uint32_t init_size = state->rom_size > state->info->save_size ? state->info->save_size : state->rom_size;
			memcpy(state->info->save_buffer, state->rom, init_size);
			if (init_size < state->info->save_size) {
				memset(state->info->save_buffer + init_size, 0xFF, state->info->save_size - init_size);
			}
			if (state->info->save_bus == RAM_FLAG_BOTH) {
				byteswap_rom(state->info->save_size, (uint16_t *)state->info->save_buffer);
			}
		} else {
			memset(state->info->save_buffer, strtol(init, NULL, 16), state->info->save_size);
		}
		state->info->nor = calloc(1, sizeof(nor_state));
		nor_flash_init(state->info->nor, state->info->save_buffer, state->info->save_size, save_page_size, save_product_id, state->info->save_bus);
		char *cmd1 = tern_find_path(state->root, "NOR\0cmd_address1\0", TVAL_PTR).ptrval;
		if (cmd1) {
			state->info->nor->cmd_address1 = strtol(cmd1, NULL, 16);
		}
		char *cmd2 = tern_find_path(state->root, "NOR\0cmd_address2\0", TVAL_PTR).ptrval;
		if (cmd2) {
			state->info->nor->cmd_address2 = strtol(cmd2, NULL, 16);
		}
	}
}

void add_eeprom_map(tern_node *node, uint32_t start, uint32_t end, map_iter_state *state)
{
	eeprom_map *eep_map = state->info->eeprom_map + state->info->num_eeprom;
	eep_map->start = start;
	eep_map->end = end;
	eep_map->sda_read_bit = 0xFF;
	tern_node * bits_read = tern_find_node(node, "bits_read");
	if (bits_read) {
		tern_foreach(bits_read, eeprom_read_fun, eep_map);
	}
	tern_node * bits_write = tern_find_node(node, "bits_write");
	if (bits_write) {
		tern_foreach(bits_write, eeprom_write_fun, eep_map);
	}
	debug_message("EEPROM address %X: sda read: %X, sda write: %X, scl: %X\n", start, eep_map->sda_read_bit, eep_map->sda_write_mask, eep_map->scl_mask);
	state->info->num_eeprom++;
}

char *map_node_common(char *key, tern_val val, uint8_t valtype, map_iter_state *state)
{
	if (valtype != TVAL_NODE) {
		fatal_error("ROM DB map entry %d with address %s is not a node\n", state->index, key);
	}
	tern_node *node = val.ptrval;
	uint32_t start = strtol(key, NULL, 16);
	uint32_t end = strtol(tern_find_ptr_default(node, "last", "0"), NULL, 16);
	if (!end || end < start) {
		fatal_error("'last' value is missing or invalid for ROM DB map entry %d with address %s\n", state->index, key);
	}
	char * dtype = tern_find_ptr_default(node, "device", "ROM");
	uint32_t offset = strtol(tern_find_ptr_default(node, "offset", "0"), NULL, 16);
	memmap_chunk *map = state->info->map + state->index;
	map->start = start;
	map->end = end + 1;

	if (!strcmp(dtype, "ROM")) {
		uint32_t expanded_size = nearest_pow2(state->rom_size);
		if (offset >= expanded_size) {
			fatal_error("offset of %X is invalid for ROM size of %X in map entry %d with addess %s\n", offset, state->rom_size, state->index, key);
		}
		map->buffer = state->rom + offset;
		map->mask = calc_mask(nearest_pow2(state->rom_size) - offset, start, end);
		if (strcmp(tern_find_ptr_default(node, "writeable", "no"), "yes")) {
			map->flags = MMAP_READ;
		} else {
			map->flags = MMAP_READ | MMAP_WRITE | MMAP_CODE;
		}
		return NULL;
	}
	if (!strcmp(dtype, "EEPROM")) {
		process_eeprom_def(key, state);
		add_eeprom_map(node, start, end, state);

		map->write_16 = write_eeprom_i2c_w;
		map->write_8 = write_eeprom_i2c_b;
		map->read_16 = read_eeprom_i2c_w;
		map->read_8 = read_eeprom_i2c_b;
		map->mask = 0xFFFFFF;
		return NULL;
	}
	if (!strcmp(dtype, "SRAM")) {
		process_sram_def(key, state);
		map->buffer = state->info->save_buffer + offset;
		map->flags = MMAP_READ | MMAP_WRITE;
		uint32_t save_size_mask = state->info->save_size;
		if (state->info->save_type == RAM_FLAG_ODD) {
			map->flags |= MMAP_ONLY_ODD;
			save_size_mask *= 2;
		} else if(state->info->save_type == RAM_FLAG_EVEN) {
			map->flags |= MMAP_ONLY_EVEN;
			save_size_mask *= 2;
		} else {
			map->flags |= MMAP_CODE;
		}
		map->mask = calc_mask(save_size_mask, start, end);
		return NULL;
	}
	if (!strcmp(dtype, "RAM")) {
		uint32_t size = strtol(tern_find_ptr_default(node, "size", "0"), NULL, 16);
		if (!size || size > map->end - map->start) {
			size = map->end - map->start;
		}
		map->buffer = calloc(size, 1);
		map->flags = MMAP_READ | MMAP_WRITE;
		char *bus = tern_find_ptr_default(node, "bus", "both");
		if (!strcmp(bus, "odd")) {
			map->flags |= MMAP_ONLY_ODD;
			size *= 2;
		} else if (!strcmp(bus, "even")) {
			map->flags |= MMAP_ONLY_EVEN;
			size *= 2;
		} else {
			map->flags |= MMAP_CODE;
		}
		map->mask = calc_mask(size, start, end);
		return NULL;
	}
	if (!strcmp(dtype, "NOR")) {
		process_nor_def(key, state);

		map->write_16 = nor_flash_write_w;
		map->write_8 = nor_flash_write_b;
		map->read_16 = nor_flash_read_w;
		map->read_8 = nor_flash_read_b;
		if (state->info->save_bus == RAM_FLAG_BOTH) {
			map->flags |= MMAP_READ_CODE | MMAP_CODE;
			map->buffer = state->info->save_buffer;
		}
		map->mask = 0xFFFFFF;
		return NULL;
	}
	if (!strcmp(dtype, "fixed")) {
		uint16_t *value =  malloc(2);
		map->buffer = value;
		map->mask = 0;
		map->flags = MMAP_READ;
		*value = strtol(tern_find_ptr_default(node, "value", "0"), NULL, 16);
		return NULL;
	}
	return dtype;
}

void map_iter_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	map_iter_state *state = data;
	char *dtype = map_node_common(key, val, valtype, state);
	if (!dtype) {
		//entry was handled by common function
		state->index++;
		return;
	}
	tern_node *node = val.ptrval;
	memmap_chunk *map = state->info->map + state->index;
	uint32_t offset = strtol(tern_find_ptr_default(node, "offset", "0"), NULL, 16);
	if (!strcmp(dtype, "LOCK-ON")) {
		rom_info lock_info;
		if (state->lock_on) {
			lock_info = configure_rom(state->rom_db, state->lock_on, state->lock_on_size, NULL, 0, NULL, 0);
		} else if (state->rom_size > map->start) {
			//This is a bit of a hack to deal with pre-combined S3&K/S2&K ROMs and S&K ROM hacks
			lock_info = configure_rom(state->rom_db, state->rom + map->start, state->rom_size - map->start, NULL, 0, NULL, 0);
		} else {
			//skip this entry if there is no lock on cartridge attached
			return;
		}
		//TODO: detect if this conflicts with host cart mapper
		state->info->mapper_start_index = lock_info.mapper_start_index;
		uint32_t matching_chunks = 0;
		for (int i = 0; i < lock_info.map_chunks; i++)
		{
			if (lock_info.map[i].start < 0xC00000 && lock_info.map[i].end > 0x200000) {
				matching_chunks++;
			}
		}
		if (matching_chunks == 0) {
			//Nothing mapped in the relevant range for the lock-on cart, ignore this mapping
			free_rom_info(&lock_info);
			return;
		} else if (matching_chunks > 1) {
			state->info->map_chunks += matching_chunks - 1;
			state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
			memset(state->info->map + state->info->map_chunks - (matching_chunks - 1), 0, sizeof(memmap_chunk) * (matching_chunks - 1));
			map = state->info->map + state->index;
		}
		for (int i = 0; i < lock_info.map_chunks; i++)
		{
			if (lock_info.map[i].start >= 0xC00000 || lock_info.map[i].end <= 0x200000) {
				continue;
			}
			*map = lock_info.map[i];
			if (map->start < 0x200000) {
				if (map->buffer) {
					uint32_t mask = (map->flags & MMAP_AUX_BUFF) ? map->aux_mask : map->mask;
					if (mask < 0x3FFFFF) {
						uint8_t *buf = map->buffer;
						buf += (0x200000 - map->start) & mask;
						map->buffer = buf;
					}
				}
				map->start = 0x200000;
			}
			map++;
			state->index++;
		}
		if (state->info->save_type == SAVE_NONE && lock_info.save_type != SAVE_NONE) {
			//main cart has no save device, but lock-on cart does
			if (state->lock_on) {
				state->info->is_save_lock_on = 1;
			}
			state->info->save_buffer = lock_info.save_buffer;
			state->info->save_size = lock_info.save_size;
			state->info->save_mask = lock_info.save_mask;
			state->info->nor = lock_info.nor;
			state->info->save_type = lock_info.save_type;
			state->info->save_bus = lock_info.save_bus;
			lock_info.save_buffer = NULL;
			lock_info.save_type = SAVE_NONE;
		}
		free_rom_info(&lock_info);
		return;
	} else if (!strcmp(dtype, "Sega mapper")) {
		state->info->mapper_type = MAPPER_SEGA;
		state->info->mapper_start_index = state->ptr_index++;
		char *variant = tern_find_ptr_default(node, "variant", "full");
		char *save_device = tern_find_path(node, "save\0device\0", TVAL_PTR).ptrval;
		if (save_device && !strcmp(save_device, "EEPROM")) {
			process_eeprom_def(key, state);
			add_eeprom_map(node, map->start & map->mask, (map->end - 1) & map->mask, state);
		} else if (save_device && !strcmp(save_device, "SRAM")) {
			process_sram_def(key, state);
		} else if(has_ram_header(state->rom, state->rom_size)) {
			//no save definition in ROM DB entry, but there is an SRAM header
			//this support is mostly to handle homebrew that uses the SSF2 product ID
			//in an attempt to signal desire for the full Sega/SSF2 mapper, but also uses SRAM unlike SSF2
			read_ram_header(state->info, state->rom);
		}
		if (!strcmp(variant, "save-only")) {
			state->info->map_chunks+=1;
			state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
			memset(state->info->map + state->info->map_chunks - 1, 0, sizeof(memmap_chunk) * 1);
			map = state->info->map + state->index;
			offset &= nearest_pow2(state->rom_size) - 1;
			map->buffer = state->rom + offset;
			map->mask = calc_mask(state->rom_size - offset, map->start, map->end - 1);
			map->ptr_index = state->info->mapper_start_index;
			map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE | MMAP_FUNC_NULL;
			if (save_device && !strcmp(save_device, "EEPROM")) {
				map->write_16 = write_eeprom_i2c_w;
				map->write_8 = write_eeprom_i2c_b;
				map->read_16 = read_eeprom_i2c_w;
				map->read_8 = read_eeprom_i2c_b;
			} else {
				map->read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[ptr_idx] == NULL
				map->read_8 = (read_8_fun)read_sram_b;
				map->write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
				map->write_8 = (write_8_fun)write_sram_area_b;
			}
			state->index++;
			map++;
		} else {
			state->info->map_chunks+=7;
			state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
			memset(state->info->map + state->info->map_chunks - 7, 0, sizeof(memmap_chunk) * 7);
			map = state->info->map + state->index;
			uint32_t start = map->start;
			for (int i = 0; i < 7; i++, state->index++, map++)
			{
				map->start = start + i * 0x80000;
				map->end = start + (i + 1) * 0x80000;
				map->mask = 0x7FFFF;
				map->buffer = state->rom + offset + i * 0x80000;
				map->ptr_index = state->ptr_index++;
				if (i < 3) {
					map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
				} else {
					map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE | MMAP_FUNC_NULL;
					if (save_device && !strcmp(save_device, "EEPROM")) {
						map->write_16 = write_eeprom_i2c_w;
						map->write_8 = write_eeprom_i2c_b;
						map->read_16 = read_eeprom_i2c_w;
						map->read_8 = read_eeprom_i2c_b;
					} else {
						map->read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
						map->read_8 = (read_8_fun)read_sram_b;
						map->write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
						map->write_8 = (write_8_fun)write_sram_area_b;
					}
				}
			}
		}
		map->start = 0xA13000;
		map->end = 0xA13100;
		map->mask = 0xFF;
		map->write_16 = (write_16_fun)write_bank_reg_w;
		map->write_8 = (write_8_fun)write_bank_reg_b;
#ifndef IS_LIB
	} else if (!strcmp(dtype, "MENU")) {
		//fake hardware for supporting menu
		map->buffer = NULL;
		map->mask = 0xFF;
		map->write_16 = menu_write_w;
		map->read_16 = menu_read_w;
#endif
	} else if (!strcmp(dtype, "multi-game")) {
		state->info->mapper_type = MAPPER_MULTI_GAME;
		state->info->mapper_start_index = state->ptr_index++;
		//make a mirror copy of the ROM so we can efficiently support arbitrary start offsets
		state->rom = realloc(state->rom, state->rom_size * 2);
		memcpy(state->rom + state->rom_size, state->rom, state->rom_size);
		state->rom_size *= 2;
		//make room for an extra map entry
		state->info->map_chunks+=1;
		state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
		memset(state->info->map + state->info->map_chunks - 1, 0, sizeof(memmap_chunk) * 1);
		map = state->info->map + state->index;
		map->buffer = state->rom;
		map->mask = calc_mask(state->rom_size, map->start, map->end - 1);
		map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
		map->ptr_index = state->info->mapper_start_index;
		map++;
		state->index++;
		map->start = 0xA13000;
		map->end = 0xA13100;
		map->mask = 0xFF;
		map->write_16 = write_multi_game_w;
		map->write_8 = write_multi_game_b;
	} else if (!strcmp(dtype, "megawifi")) {
		if (!strcmp(
			"on",
			tern_find_path_default(config, "system\0megawifi\0", (tern_val){.ptrval="off"}, TVAL_PTR).ptrval)
		) {
			map->write_16 = megawifi_write_w;
			map->write_8 = megawifi_write_b;
			map->read_16 = megawifi_read_w;
			map->read_8 = megawifi_read_b;
			map->mask = 0xFFFFFF;
		} else {
			warning("ROM uses MegaWiFi, but it is disabled\n");
			return;
		}
	} else if (!strcmp(dtype, "jcart")) {
		state->info->mapper_type = MAPPER_JCART;
		map->write_16 = jcart_write_w;
		map->write_8 = jcart_write_b;
		map->read_16 = jcart_read_w;
		map->read_8 = jcart_read_b;
		map->mask = 0xFFFFFF;
	} else if (!strcmp(dtype, "sft-wukong-fixed") || !strcmp(dtype, "sft-wukong-remap")) {
		state->info->mapper_type = MAPPER_SFT_WUKONG;
		uint32_t expanded_size = nearest_pow2(state->rom_size);
		if (offset >= expanded_size) {
			fatal_error("offset of %X is invalid for ROM size of %X in map entry %d with addess %s\n", offset, state->rom_size, state->index, key);
		}
		map->buffer = state->rom + offset;
		map->mask = calc_mask(nearest_pow2(state->rom_size) - offset, map->start, map->end - 1);
		map->write_8 = sft_wukong_write_b;
		map->write_16 = sft_wukong_write_w;
		if (!strcmp(dtype, "sft-wukong-remap")) {
			map->flags = MMAP_READ | MMAP_CODE | MMAP_PTR_IDX;
			state->info->mapper_start_index = state->ptr_index++;
		} else {
			map->flags = MMAP_READ;
		}
	} else {
		fatal_error("Invalid device type %s for ROM DB map entry %d with address %s\n", dtype, state->index, key);
	}
	state->index++;
}

void handle_io_overrides(tern_node *entry, rom_info *info)
{
	tern_node *device_overrides = tern_find_node(entry, "device_overrides");
	if (device_overrides) {
		info->port1_override = tern_find_ptr(device_overrides, "1");
		info->port2_override = tern_find_ptr(device_overrides, "2");
		info->ext_override = tern_find_ptr(device_overrides, "ext");
		if (
			info->save_type == SAVE_NONE
			&& (
				(info->port1_override && startswith(info->port1_override, "heartbeat_trainer."))
				|| (info->port2_override && startswith(info->port2_override, "heartbeat_trainer."))
				|| (info->ext_override && startswith(info->ext_override, "heartbeat_trainer."))
			)
		) {
			info->save_type = SAVE_HBPT;
			info->save_size = atoi(tern_find_path_default(entry, "HeartbeatTrainer\0size\0", (tern_val){.ptrval="512"}, TVAL_PTR).ptrval);
			info->save_buffer = calloc(info->save_size + 5 + 8, 1);
			memset(info->save_buffer, 0xFF, info->save_size);
		}
	} else {
		info->port1_override = info->port2_override = info->ext_override = NULL;
	}
}

rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	uint8_t product_id[GAME_ID_LEN+1];
	uint8_t *rom = vrom;
	uint32_t expanded_size = nearest_pow2(rom_size);
	if (expanded_size > rom_size) {
		//generally carts with odd-sized ROMs have 2 power of 2 sized ROMs with the larger one first
		//TODO: Handle cases in which the 2nd ROM/part is a maller power of 2 than just half the first one
		uint32_t mirror_start = expanded_size >> 1;
		uint32_t mirror_size = expanded_size >> 2;
		if (mirror_start + mirror_size >= rom_size) {
			memcpy(rom + mirror_start + mirror_size, rom + mirror_start, mirror_size);
		}
	}
	product_id[GAME_ID_LEN] = 0;
	for (int i = 0; i < GAME_ID_LEN; i++)
	{
		if (i >= 3 && rom[GAME_ID_OFF + i] <= ' ') {
			product_id[i] = 0;
			break;
		}
		product_id[i] = rom[GAME_ID_OFF + i];

	}
	debug_message("Product ID: %s\n", product_id);
	uint8_t raw_hash[20];
	sha1(vrom, rom_size, raw_hash);
	uint8_t hex_hash[41];
	bin_to_hex(hex_hash, raw_hash, 20);
	debug_message("SHA1: %s\n", hex_hash);
	tern_node * entry = tern_find_node(rom_db, hex_hash);
	if (!entry) {
		entry = tern_find_node(rom_db, product_id);
	}
	if (!entry) {
		entry = tern_find_node(rom_db, product_id + 3);
	}
	if (!entry) {
		debug_message("Not found in ROM DB, examining header\n\n");
		if (xband_detect(rom, rom_size)) {
			return xband_configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, base_map, base_chunks);
		}
		if (realtec_detect(rom, rom_size)) {
			return realtec_configure_rom(rom, rom_size, base_map, base_chunks);
		}
		return configure_rom_heuristics(rom, rom_size, base_map, base_chunks);
	}
	rom_info info;
	info.mapper_type = MAPPER_NONE;
	info.name = tern_find_ptr(entry, "name");
	if (info.name) {
		debug_message("Found name: %s\n\n", info.name);
		info.name = strdup(info.name);
	} else {
		info.name = get_header_name(rom);
	}

	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	if (!info.regions) {
		info.regions = get_header_regions(rom);
	}

	info.is_save_lock_on = 0;
	info.rom = vrom;
	info.rom_size = rom_size;
	tern_node *map = tern_find_node(entry, "map");
	if (map) {
		info.save_type = SAVE_NONE;
		info.map_chunks = tern_count(map);
		if (info.map_chunks) {
			info.map_chunks += base_chunks;
			info.save_buffer = NULL;
			info.save_size = 0;
			info.map = malloc(sizeof(memmap_chunk) * info.map_chunks);
			info.eeprom_map = NULL;
			info.num_eeprom = 0;
			memset(info.map, 0, sizeof(memmap_chunk) * info.map_chunks);
			map_iter_state state = {
				.info = &info,
				.rom = rom,
				.lock_on = lock_on,
				.root = entry,
				.rom_db = rom_db,
				.rom_size = rom_size,
				.lock_on_size = lock_on_size,
				.index = 0,
				.num_els = info.map_chunks - base_chunks,
				.ptr_index = 0
			};
			tern_foreach(map, map_iter_fun, &state);
			memcpy(info.map + state.index, base_map, sizeof(memmap_chunk) * base_chunks);
			info.rom = state.rom;
			info.rom_size = state.rom_size;
		} else {
			add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
		}
	} else {
		add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	}

	handle_io_overrides(entry, &info);
	info.mouse_mode = tern_find_ptr(entry, "mouse_mode");
	info.wants_cd = !strcmp(tern_find_ptr_default(entry, "wants_cd", "no"), "yes");

	return info;
}

void *sms_sega_mapper_write(uint32_t location, void *vcontext, uint8_t value);
void *sms_cart_ram_write(uint32_t location, void *vcontext, uint8_t value);
void *sms_codemasters_bank0_write(uint32_t location, void *vcontext, uint8_t value);
void *sms_codemasters_bank1_write(uint32_t location, void *vcontext, uint8_t value);
void *sms_codemasters_bank2_write(uint32_t location, void *vcontext, uint8_t value);
void map_iter_fun_sms(char *key, tern_val val, uint8_t valtype, void *data)
{
	map_iter_state *state = data;
	char *dtype = map_node_common(key, val, valtype, state);
	if (!dtype) {
		//entry was handled by common function
		state->index++;
		return;
	}
	tern_node *node = val.ptrval;
	memmap_chunk *map = state->info->map + state->index;
	uint32_t offset = strtol(tern_find_ptr_default(node, "offset", "0"), NULL, 16);
	if (!strcmp(dtype, "Sega mapper")) {
		state->info->mapper_type = MAPPER_SMS_SEGA;
		state->info->mapper_start_index = state->ptr_index++;
		char *save_device = tern_find_path(node, "save\0device\0", TVAL_PTR).ptrval;
		if (save_device && !strcmp(save_device, "EEPROM")) {
			process_eeprom_def(key, state);
			add_eeprom_map(node, map->start & map->mask, (map->end - 1) & map->mask, state);
		} else if (save_device && !strcmp(save_device, "SRAM")) {
			process_sram_def(key, state);
		}
		state->info->map_chunks += 4;
		state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
		map = state->info->map + state->index;
		map[0].start = 0;
		map[0].end = 0x400;
		map[0].mask = 0xFFFF;
		map[0].flags = MMAP_READ;
		map[0].buffer = state->info->rom;
		map[1].start = 0x400;
		map[1].end = 0x4000;
		map[1].mask = 0x3FFF;
		map[1].ptr_index = 0;
		map[1].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
		map[2].start = 0x4000;
		map[2].end = 0x8000;
		map[2].mask = 0x3FFF;
		map[2].ptr_index = 1;
		map[2].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
		map[3].start = 0x8000;
		map[3].end = 0xC000;
		map[3].mask = 0x3FFF;
		map[3].ptr_index = 2;
		if (state->info->save_type == RAM_FLAG_ODD || state->info->save_type == RAM_FLAG_EVEN) {
			map[3].write_8 = sms_cart_ram_write;
		}
		map[3].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
		map[4].start = 0xFFFC;
		map[4].end = 0x10000;
		map[4].mask = 3;
		map[4].flags = MMAP_READ;
		map[4].write_8 = sms_sega_mapper_write;
	} else {
		fatal_error("Invalid device type %s for ROM DB map entry %d with address %s\n", dtype, state->index, key);
	}
}

uint8_t has_codemasters_header(uint8_t *rom, uint32_t rom_size)
{
	if (rom_size < 0x8000) {
		return 0;
	}
	//Date and time fields should be valid BCD
	for (uint32_t i = 0x7FE1; i < 0x7FE6; i++)
	{
		if ((rom[i] & 0xF0) > 0x90) {
			return 0;
		}
		if ((rom[i] & 0xF) > 0x9) {
			return 0;
		}
	}
	uint16_t checksum = rom[0x7FE6] | rom[0x7FE7] << 8;
	uint16_t complement = rom[0x7FE8] | rom[0x7FE9] << 8;
	if (complement != (0x10000 - checksum)) {
		return 0;
	}
	puts("Detected codemasters mapper");
	return 1;
}

void sms_memmap_heuristics(rom_info *info, memmap_chunk const *base_map, uint32_t num_base_chunks)
{
	uint32_t num_chunks = num_base_chunks + (info->rom_size > 0xC000 ? 5 : 1);
	memmap_chunk *chunks = calloc(num_chunks, sizeof(memmap_chunk));
	info->map = chunks;
	info->map_chunks = num_chunks;
	if (info->rom_size > 0xC000) {
		if (has_codemasters_header(info->rom, info->rom_size)) {
			info->mapper_type = MAPPER_SMS_CODEMASTERS;
			memcpy(chunks + 3, base_map, sizeof(memmap_chunk) * num_base_chunks);
			num_chunks--;
			chunks[0].start = 0;
			chunks[0].end = 0x4000;
			chunks[0].mask = 0x3FFF;
			chunks[0].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[0].ptr_index = 0;
			chunks[0].write_8 = sms_codemasters_bank0_write;
			chunks[1].start = 0x4000;
			chunks[1].end = 0x8000;
			chunks[1].mask = 0x3FFF;
			chunks[1].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[1].ptr_index = 1;
			chunks[1].write_8 = sms_codemasters_bank1_write;
			chunks[2].start = 0x8000;
			chunks[2].end = 0xC000;
			chunks[2].mask = 0x3FFF;
			chunks[2].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[2].ptr_index = 2;
			chunks[2].write_8 = sms_codemasters_bank2_write;
		} else {
			info->mapper_type = MAPPER_SMS_SEGA;
			memcpy(chunks + 4, base_map, sizeof(memmap_chunk) * num_base_chunks);
			chunks[0].start = 0;
			chunks[0].end = 0x400;
			chunks[0].mask = 0xFFFF;
			chunks[0].flags = MMAP_READ;
			chunks[0].buffer = info->rom;
			chunks[1].start = 0x400;
			chunks[1].end = 0x4000;
			chunks[1].mask = 0x3FFF;
			chunks[1].ptr_index = 0;
			chunks[1].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[2].start = 0x4000;
			chunks[2].end = 0x8000;
			chunks[2].mask = 0x3FFF;
			chunks[2].ptr_index = 1;
			chunks[2].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[3].start = 0x8000;
			chunks[3].end = 0xC000;
			chunks[3].mask = 0x3FFF;
			chunks[3].ptr_index = 2;
			chunks[3].flags = MMAP_READ|MMAP_PTR_IDX|MMAP_CODE;
			chunks[3].write_8 = sms_cart_ram_write;
			chunks[num_chunks - 1].start = 0xFFFC;
			chunks[num_chunks - 1].end = 0x10000;
			chunks[num_chunks - 1].mask = 3;
			chunks[num_chunks - 1].flags = MMAP_READ;
			chunks[num_chunks - 1].write_8 = sms_sega_mapper_write;
			for (uint32_t i = 4; i < num_chunks - 1; i++)
			{
				if (chunks[i].end > 0xFFFC) {
					//mapper regs overlap RAM from base map
					chunks[i].end = 0xFFFC;
				}
			}
		}
	} else {
		info->mapper_type = MAPPER_NONE;
		memcpy(chunks + 1, base_map, sizeof(memmap_chunk) * num_base_chunks);
		chunks[0].start = 0;
		chunks[0].end = 0xC000;
		chunks[0].mask = nearest_pow2(info->rom_size)-1;
		chunks[0].flags = MMAP_READ;
		chunks[0].buffer = info->rom;
	}
}

void configure_rom_sms_heuristics(rom_info *info, uint32_t header_offset, memmap_chunk const *base_map, uint32_t num_base_chunks)
{
	sms_memmap_heuristics(info, base_map, num_base_chunks);
}

uint8_t check_sms_sega_header(uint8_t *rom, char *product_code, uint32_t offset)
{
	if (memcmp(rom + offset, "TMR SEGA", strlen("TMR SEGA"))) {
		return 0;
	}
	char *cur = product_code + 4;
	uint8_t begin = rom[offset + 0xE] >> 4;
	if (begin < 0xA) {
		*(cur++) = begin + '0';
	} else {
		*(cur++) = '1';
		*(cur++) = begin - 0xA + '0';
	}
	uint8_t *src = rom + offset + 0xD;
	for (int i = 0; i < 2; i++, src--)
	{
		*(cur++) = (*src >> 4) + '0';
		*(cur++) = (*src & 0xF) + '0';
	}
	*cur = 0;
	return 1;
}

rom_info configure_rom_sms(tern_node *rom_db, uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_chunks, uint32_t num_base_chunks)
{
	uint32_t expanded_size = nearest_pow2(rom_size);
	if (expanded_size > rom_size) {
		//generally carts with odd-sized ROMs have 2 power of 2 sized ROMs with the larger one first
		//TODO: Handle cases in which the 2nd ROM/part is a maller power of 2 than just half the first one
		uint32_t mirror_start = expanded_size >> 1;
		uint32_t mirror_size = expanded_size >> 2;
		if (mirror_start + mirror_size >= rom_size) {
			memcpy(rom + mirror_start + mirror_size, rom + mirror_start, mirror_size);
		}
	}
	char product_code[] = "sms:000000";
	uint8_t found_header = 0;
	uint32_t offset = 0;
	if (rom_size >= 0x8000) {
		offset = 0x7FF0;
		found_header = check_sms_sega_header(rom, product_code, offset);
	}
	if (!found_header && rom_size >= 0x4000) {
		offset = 0x3FF0;
		found_header = check_sms_sega_header(rom, product_code, offset);
	}
	if (!found_header && rom_size >= 0x2000) {
		offset = 0x1FF0;
		found_header = check_sms_sega_header(rom, product_code, offset);
	}
	debug_message("Product Code: %s\n", product_code);
	uint8_t raw_hash[20];
	sha1(rom, rom_size, raw_hash);
	uint8_t hex_hash[41];
	bin_to_hex(hex_hash, raw_hash, 20);
	debug_message("SHA1: %s\n", hex_hash);
	tern_node * entry = tern_find_node(rom_db, hex_hash);
	if (!entry) {
		entry = tern_find_node(rom_db, product_code);
	}
	rom_info info = {0};
	info.rom_size = rom_size;
	info.rom = rom;
	if (!entry) {
		debug_message("Not found in ROM DB, examining header\n\n");
		configure_rom_sms_heuristics(&info, offset, base_chunks, num_base_chunks);
		return info;
	}
	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	tern_node *map = tern_find_node(entry, "map");
	if (map) {
		info.save_type = SAVE_NONE;
		info.map_chunks = tern_count(map);
		if (info.map_chunks) {
			info.map_chunks += num_base_chunks;
			info.save_buffer = NULL;
			info.save_size = 0;
			info.map = malloc(sizeof(memmap_chunk) * info.map_chunks);
			info.eeprom_map = NULL;
			info.num_eeprom = 0;
			memset(info.map, 0, sizeof(memmap_chunk) * info.map_chunks);
			map_iter_state state = {
				.info = &info,
				.rom = rom,
				.root = entry,
				.rom_db = rom_db,
				.rom_size = rom_size,
				.index = 0,
				.num_els = info.map_chunks - num_base_chunks,
				.ptr_index = 0
			};
			tern_foreach(map, map_iter_fun_sms, &state);
			memcpy(info.map + state.index, base_chunks, sizeof(memmap_chunk) * num_base_chunks);
			info.rom = state.rom;
			info.rom_size = state.rom_size;
		} else {
			sms_memmap_heuristics(&info, base_chunks, num_base_chunks);
		}
	} else {
		sms_memmap_heuristics(&info, base_chunks, num_base_chunks);
	}
	handle_io_overrides(entry, &info);
	return info;
}
