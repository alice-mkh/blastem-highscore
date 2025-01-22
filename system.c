#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "genesis.h"
#include "gen_player.h"
#include "sms.h"
#include "mediaplayer.h"
#include "coleco.h"
#include "paths.h"
#include "util.h"
#include "cdimage.h"

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

#ifdef DISABLE_ZLIB
#define ROMFILE FILE*
#define romopen fopen
#define romread fread
#define romseek fseek
#define romgetc fgetc
#define romclose fclose
#else
#include "zlib/zlib.h"
#define ROMFILE gzFile
#define romopen gzopen
#define romread gzfread
#define romseek gzseek
#define romgetc gzgetc
#define romclose gzclose
#endif

uint16_t *process_smd_block(uint16_t *dst, uint8_t *src, size_t bytes)
{
	for (uint8_t *low = src, *high = (src+bytes/2), *end = src+bytes; high < end; high++, low++) {
		*(dst++) = *low << 8 | *high;
	}
	return dst;
}

int load_smd_rom(ROMFILE f, void **buffer)
{
	uint8_t block[SMD_BLOCK_SIZE];
	romseek(f, SMD_HEADER_SIZE, SEEK_SET);

	size_t filesize = 512 * 1024;
	size_t readsize = 0;
	uint16_t *dst, *buf;
	dst = buf = malloc(filesize);


	size_t read;
	do {
		if ((readsize + SMD_BLOCK_SIZE > filesize)) {
			filesize *= 2;
			buf = realloc(buf, filesize);
			dst = buf + readsize/sizeof(uint16_t);
		}
		read = romread(block, 1, SMD_BLOCK_SIZE, f);
		if (read > 0) {
			dst = process_smd_block(dst, block, read);
			readsize += read;
		}
	} while(read > 0);
	romclose(f);

	*buffer = buf;

	return readsize;
}

uint8_t is_smd_format(const char *filename, uint8_t *header)
{
	if (header[1] == SMD_MAGIC1 && header[8] == SMD_MAGIC2 && header[9] == SMD_MAGIC3) {
		int i;
		for (i = 3; i < 8; i++) {
			if (header[i] != 0) {
				return 0;
			}
		}
		if (i == 8) {
			if (header[2]) {
				fatal_error("%s is a split SMD ROM which is not currently supported", filename);
			}
			return 1;
		}
	}
	return 0;
}

#ifndef IS_LIB
uint32_t load_media_zip(const char *filename, system_media *dst)
{
	static const char *valid_exts[] = {"bin", "md", "gen", "sms", "gg", "rom", "smd", "sg", "sc", "sf7"};
	const uint32_t num_exts = sizeof(valid_exts)/sizeof(*valid_exts);
	zip_file *z = zip_open(filename);
	if (!z) {
		return 0;
	}

	for (uint32_t i = 0; i < z->num_entries; i++)
	{
		char *ext = path_extension(z->entries[i].name);
		if (!ext) {
			continue;
		}
		for (uint32_t j = 0; j < num_exts; j++)
		{
			if (!strcasecmp(ext, valid_exts[j])) {
				size_t out_size = nearest_pow2(z->entries[i].size);
				dst->buffer = zip_read(z, i, &out_size);
				if (dst->buffer) {
					if (is_smd_format(z->entries[i].name, dst->buffer)) {
						size_t offset;
						for (offset = 0; offset + SMD_BLOCK_SIZE + SMD_HEADER_SIZE <= out_size; offset += SMD_BLOCK_SIZE)
						{
							uint8_t tmp[SMD_BLOCK_SIZE];
							uint8_t *u8dst = dst->buffer;
							memcpy(tmp, u8dst + offset + SMD_HEADER_SIZE, SMD_BLOCK_SIZE);
							process_smd_block((void *)(u8dst + offset), tmp, SMD_BLOCK_SIZE);
						}
						out_size = offset;
					}
					dst->extension = ext;
					dst->dir = path_dirname(filename);
					if (!dst->dir) {
						dst->dir = path_current_dir();
					}
					dst->name = basename_no_extension(filename);
					dst->size = out_size;
					dst->zip = z;
					return out_size;
				}
			}
		}
		free(ext);
	}
	zip_close(z);
	return 0;
}
#endif

uint32_t load_media(char * filename, system_media *dst, system_type *stype)
{
	uint8_t header[10];
#ifndef IS_LIB
	if (dst->zip) {
		zip_close(dst->zip);
		dst->zip = NULL;
	}
#endif
	dst->orig_path = filename;
	char *ext = path_extension(filename);
#ifndef IS_LIB
	if (ext && !strcasecmp(ext, "zip")) {
		free(ext);
		return load_media_zip(filename, dst);
	}
#endif
	if (ext && !strcasecmp(ext, "iso")) {
		if (stype) {
			*stype = SYSTEM_SEGACD;
		}
		return make_iso_media(dst, filename);
	}

	ROMFILE f = romopen(filename, "rb");
	if (!f) {
		free(ext);
		return 0;
	}
#ifndef DISABLE_ZLIB
	char *to_free = NULL;
	if (!gzdirect(f) && ext && !strcasecmp(ext, "gz")) {
		size_t without_gz = strlen(filename) - 2;
		to_free = calloc(1, without_gz);
		memcpy(to_free, filename, without_gz - 1);
		to_free[without_gz - 1] = 0;
		free(ext);
		filename = to_free;
		ext = path_extension(filename);
	}
#endif //DISABLE_ZLIB

	if (sizeof(header) != romread(header, 1, sizeof(header), f)) {
		fatal_error("Error reading from %s\n", filename);
	}

	uint32_t ret = 0;
	if (is_smd_format(filename, header)) {
		if (stype) {
			*stype = SYSTEM_GENESIS;
		}
		ret = load_smd_rom(f, &dst->buffer);
	}

	if (!ret) {
		size_t filesize = 512 * 1024;
		size_t readsize = sizeof(header);

		char *buf = malloc(filesize);
		memcpy(buf, header, readsize);

		size_t read;
		do {
			read = romread(buf + readsize, 1, filesize - readsize, f);
			if (read > 0) {
				readsize += read;
				if (readsize == filesize) {
					int one_more = romgetc(f);
					if (one_more >= 0) {
						filesize *= 2;
						buf = realloc(buf, filesize);
						buf[readsize++] = one_more;
					} else {
						read = 0;
					}
				}
			}
		} while (read > 0);
		dst->buffer = buf;
		ret = (uint32_t)readsize;
	}
	dst->dir = path_dirname(filename);
	if (!dst->dir) {
		dst->dir = path_current_dir();
	}
	dst->name = basename_no_extension(filename);
	dst->extension = ext;
	dst->size = ret;
	romclose(f);
	if (!strcasecmp(dst->extension, "cue")) {
		if (parse_cue(dst)) {
			ret = dst->size;
			if (stype) {
				*stype = SYSTEM_SEGACD;
			}
		}
	} else if (!strcasecmp(dst->extension, "toc")) {
		if (parse_toc(dst)) {
			ret = dst->size;
			if (stype) {
				*stype = SYSTEM_SEGACD;
			}
		}
	}
#ifndef DISABLE_ZLIB
	if (to_free) {
		free(to_free);
	}
#endif

	return ret;
}

uint8_t safe_cmp(char *str, long offset, uint8_t *buffer, long filesize)
{
	long len = strlen(str);
	return filesize >= offset+len && !memcmp(str, buffer + offset, len);
}

system_type detect_system_type(system_media *media)
{
	static char *pico_names[] = {
		"SEGA PICO", "SEGATOYS PICO", "SEGA TOYS PICO", "SAMSUNG PICO",
		"SEGA IAC", "IMA IKUNOUJYUKU", "IMA IKUNOJYUKU"
	};
	static const int num_pico = sizeof(pico_names)/sizeof(*pico_names);
	for (int i = 0; i < num_pico; i++) {
		if (safe_cmp(pico_names[i], 0x100, media->buffer, media->size)) {
			return SYSTEM_PICO;
		}
	}
	if (safe_cmp("SEGA", 0x100, media->buffer, media->size)) {
		//TODO: support other bootable identifiers
		if (safe_cmp("SEGADISCSYSTEM", 0, media->buffer, media->size)) {
			return SYSTEM_SEGACD;
		}
		//TODO: Differentiate between vanilla Genesis and 32X games
		return SYSTEM_GENESIS;
	}
	if (safe_cmp("TMR SEGA", 0x1FF0, media->buffer, media->size)
		|| safe_cmp("TMR SEGA", 0x3FF0, media->buffer, media->size)
		|| safe_cmp("TMR SEGA", 0x7FF0, media->buffer, media->size)
	) {
		return SYSTEM_SMS;
	}
	if (media->size > 400) {
		uint8_t *buffer = media->buffer;
		if (!memcmp(buffer + 4, "\x00\x00\x04\x00", 4) && (buffer[0x80] == 0 || buffer[0x80] == 0xFF)) {
			int i = 0x81;
			for(; i < 0x400; i++)
			{
				if (buffer[i] != buffer[0x80]) {
					break;
				}
			}
			if (i == 0x400) {
				return SYSTEM_COPERA;
			}
		}
	}
	if (safe_cmp("BLSTEL\x02", 0, media->buffer, media->size)) {
		uint8_t *buffer = media->buffer;
		if (media->size > 9 && buffer[7] == 0) {
			return buffer[8] + 1;
		}
	}
	if (
		safe_cmp("Vgm ", 0, media->buffer, media->size)
		|| safe_cmp("RIFF", 0, media->buffer, media->size)
		|| safe_cmp("fLaC", 0, media->buffer, media->size)) {
		return SYSTEM_MEDIA_PLAYER;
	}
	if (
		(safe_cmp("\xAA\x55", 0, media->buffer, media->size)
		|| safe_cmp("\x55\xAA", 0, media->buffer, media->size))
		&& media->size > 0xB) {
		uint8_t *buffer = media->buffer;
		if (((buffer[0xB] << 8) | buffer[0xA]) > 0x8000) {
			return SYSTEM_COLECOVISION;
		}
	}


	//TODO: Detect Jaguar ROMs here

	//Header based detection failed, examine filename for clues
	if (media->extension) {
		if (!strcmp("md", media->extension) || !strcmp("gen", media->extension)) {
			return SYSTEM_GENESIS;
		}
		if (!strcmp("sms", media->extension) || !strcmp("sg", media->extension) || !strcmp("gg", media->extension)
			|| !strcmp("sc", media->extension) || !strcmp("sf7", media->extension)) {
			return SYSTEM_SMS;
		}
		if (!strcmp("j64", media->extension)) {
			return SYSTEM_JAGUAR;
		}
		if (!strcmp("col", media->extension)) {
			return SYSTEM_COLECOVISION;
		}
	}

	//More certain checks failed, look for a valid 68K reset vector
	if (media->size >= 8) {
		char *rom = media->buffer;
		uint32_t reset = rom[5] << 16 | rom[6] << 8 | rom[7];
		if (!(reset & 1) && reset < media->size) {
			//we have a valid looking reset vector, assume it's a Genesis ROM
			return SYSTEM_GENESIS;
		}
	}
	return SYSTEM_UNKNOWN;
}

system_header *alloc_config_system(system_type stype, system_media *media, uint32_t opts, uint8_t force_region)
{
	void *lock_on = NULL;
	uint32_t lock_on_size = 0;
	if (media->chain) {
		lock_on = media->chain->buffer;
		lock_on_size = media->chain->size;
	}
	switch (stype)
	{
	case SYSTEM_GENESIS:
		return &(alloc_config_genesis(media->buffer, media->size, lock_on, lock_on_size, opts, force_region))->header;
	case SYSTEM_GENESIS_PLAYER:
		return &(alloc_config_gen_player(media->buffer, media->size))->header;
	case SYSTEM_SEGACD:
		return &(alloc_config_genesis_cdboot(media, opts, force_region))->header;
#ifndef NO_Z80
	case SYSTEM_SMS:
		return &(alloc_configure_sms(media, opts, force_region))->header;
	case SYSTEM_COLECOVISION:
		return &(alloc_configure_coleco(media))->header;
#endif
	case SYSTEM_MEDIA_PLAYER:
		return &(alloc_media_player(media, opts))->header;
	case SYSTEM_PICO:
	case SYSTEM_COPERA:
		return &(alloc_config_pico(media->buffer, media->size, lock_on, lock_on_size, opts, force_region, stype))->header;
	default:
		return NULL;
	}
}

system_header *alloc_config_player(system_type stype, event_reader *reader)
{
	switch(stype)
	{
	case SYSTEM_GENESIS:
		return &(alloc_config_gen_player_reader(reader))->header;
	}
	return NULL;
}

void system_request_exit(system_header *system, uint8_t force_release)
{
	system->force_release = force_release;
	system->request_exit(system);
}

void* load_media_subfile(const system_media *media, char *path, uint32_t *sizeout)
{
#ifdef IS_LIB
	//TODO: Figure out how to handle Pico artwork and similar cases in libretro builds
	return NULL;
#else
	char *to_free = NULL;
	void *buffer = NULL;
	uint32_t size = 0;
	if (media->zip) {
		uint32_t i;
		for (i = 0; i < media->zip->num_entries; i++)
		{
			if (!strcasecmp(media->zip->entries[i].name, path)) {
				break;
			}
		}
		if (i < media->zip->num_entries) {
			size_t zsize = media->zip->entries[i].size + 1;
			buffer = zip_read(media->zip, i, &zsize);
			size = zsize;
			if (buffer) {
				((uint8_t *)buffer)[size] = 0;
			}
			goto end;
		}
	}
	if (!is_absolute_path(path)) {
		to_free = path = path_append(media->dir, path);
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		goto end;
	}
	size = file_size(f);
	buffer = calloc(1, size + 1);
	size = fread(buffer, 1, size, f);
	fclose(f);
	
end:
	if (sizeout) {
		*sizeout = size;
	}
	free(to_free);
	return buffer;
#endif
}
