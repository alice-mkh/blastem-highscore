#include "disasm.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

label_def *find_label(disasm_context *context, uint32_t address)
{
	char key[MAX_INT_KEY_SIZE];
	tern_sortable_int_key(address & context->address_mask, key);
	return tern_find_ptr(context->labels, key);
}

int format_label(char *dst, uint32_t address, disasm_context *context)
{
	label_def *def = find_label(context, address);
	if (def && def->num_labels) {
		return sprintf(dst, "%s", def->labels[0]);
	}
	return sprintf(dst, "ADR_%X", address);
}

label_def *add_find_label(disasm_context *context, uint32_t address)
{
	char key[MAX_INT_KEY_SIZE];
	tern_sortable_int_key(address & context->address_mask, key);
	label_def *def = tern_find_ptr(context->labels, key);
	if (!def)
	{
		def = calloc(1, sizeof(label_def));
		context->labels = tern_insert_ptr(context->labels, key, def);
	}
	def->full_address = address;
	return def;
}

void weak_label(disasm_context *context, const char *name, uint32_t address)
{
	label_def *def = add_find_label(context, address);
	if (def->num_labels == def->storage) {
		def->storage = def->storage ? def->storage * 2 : 4;
		def->labels = realloc(def->labels, def->storage * sizeof(char*));;
	}
	def->labels[def->num_labels++] = strdup(name);
}

void reference(disasm_context *context, uint32_t address)
{
	label_def *def = add_find_label(context, address);
	def->referenced = 1;
}

void add_label(disasm_context *context, const char *name, uint32_t address)
{
	reference(context, address);;
	weak_label(context, name, address);
}

void visit(disasm_context *context, uint32_t address)
{
	if (!context->visited) {
		uint32_t size = context->address_mask + 1;
		size >>= context->visit_preshift;
		size >>= 3;
		context->visited = calloc(1, size);
	}
	address &= context->address_mask;
	address >>= context->visit_preshift;
	context->visited[address >> 3] |= 1 << (address & 7);
}

uint8_t is_visited(disasm_context *context, uint32_t address)
{
	if (!context->visited) {
		return 0;
	}
	address &= context->address_mask;
	address >>= context->visit_preshift;
	return (context->visited[address >> 3] & (1 << (address & 7))) != 0;
}

void defer_disasm(disasm_context *context, uint32_t address)
{
	if (is_visited(context, address) || address & context->invalid_inst_addr_mask) {
		return;
	}
	context->deferred = defer_address(context->deferred, address, NULL);
}

void process_m68k_vectors(disasm_context *context, uint16_t *table, uint8_t labels_only)
{
	static const char* names[] = {
		"access_fault",
		"address_error",
		"illegal_instruction",
		"integer_divide_by_zero",
		"chk_exception",
		"trap_exception",
		"privilege_violation",
		"trace_exception",
		"line_1010_emulator",
		"line_1111_emulator"
	};
	uint32_t address = table[2] << 16 | table[3];
	add_label(context, "start", address);
	if (!labels_only) {
		defer_disasm(context, address);
	}
	for (int i = 0; i < sizeof(names)/sizeof(*names); i++)
	{
		address = table[i*2+4] << 16 | table[i*2 + 5];
		add_label(context, names[i], address);
		if (!labels_only) {
			defer_disasm(context, address);
		}
	}
	char int_name[] = "int_0";
	for (int i = 0; i < 7; i++)
	{
		int_name[4] = '1' + i;
		address = table[i*2+50] << 16 | table[i*2 + 51];
		add_label(context, int_name, address);
		if (!labels_only) {
			defer_disasm(context, address);
		}
	}

	char trap_name[] = "trap_0";
	for (int i = 0; i < 16; i++)
	{
		trap_name[5] = i < 0xA ? '0' + i : 'a' + i - 0xA;
		address = table[i*2+50] << 16 | table[i*2 + 51];
		add_label(context, trap_name, address);
		if (!labels_only) {
			defer_disasm(context, address);
		}
	}
}

void add_segacd_maincpu_labels(disasm_context *context)
{
	weak_label(context, "_bios_reset", 0x280);
	weak_label(context, "_bios_entry", 0x284);
	weak_label(context, "_bios_init", 0x288);
	weak_label(context, "_bios_init_sp", 0x28C);
	weak_label(context, "_bios_vint", 0x290);
	weak_label(context, "_bios_set_hint", 0x294);
	weak_label(context, "_bios_poll_io", 0x298);
	weak_label(context, "_bios_detect_io", 0x29C);
	weak_label(context, "_bios_clear_vram", 0x2A0);
	weak_label(context, "_bios_clear_nmtbl", 0x2A4);
	weak_label(context, "_bios_clear_vsram", 0x2A8);
	weak_label(context, "_bios_init_vdp", 0x2AC);
	weak_label(context, "_bios_vdp_loadregs", 0x2B0);
	weak_label(context, "_bios_vdp_fill", 0x2B4);
	weak_label(context, "_bios_clear_vram_range", 0x2B8);
	weak_label(context, "_bios_clear_vram_range_dma", 0x2BC);
	weak_label(context, "_bios_vram_dma_fill", 0x2C0);
	weak_label(context, "_bios_update_nmtbl", 0x2C4);
	weak_label(context, "_bios_update_nmtbl_template", 0x2C8);
	weak_label(context, "_bios_fill_nmtbl", 0x2CC);
	weak_label(context, "_bios_vdp_dma", 0x2D0);
	weak_label(context, "_bios_vdp_dma_wordram", 0x2D4);
	weak_label(context, "_bios_vdp_display_enable", 0x2D8);
	weak_label(context, "_bios_vdp_display_disable", 0x2DC);
	weak_label(context, "_bios_pal_buffer", 0x2E0);
	weak_label(context, "_bios_pal_buffer_update", 0x2E4);
	weak_label(context, "_bios_pal_dma", 0x2E8);
	weak_label(context, "_bios_gfx_decomp", 0x2EC);
	weak_label(context, "_bios_gfx_decomp_ram", 0x2F0);
	weak_label(context, "_bios_update_sprites", 0x2F4);
	weak_label(context, "_bios_clear_ram", 0x2F8);
	weak_label(context, "_bios_display_sprite", 0x300);
	weak_label(context, "_bios_wait_vint", 0x304);
	weak_label(context, "_bios_wait_vint_flags", 0x308);
	weak_label(context, "_bios_dma_sat", 0x30C);
	weak_label(context, "_bios_set_hint_direct", 0x314);
	weak_label(context, "_bios_disable_hint", 0x318);
	weak_label(context, "_bios_print", 0x31C);
	weak_label(context, "_bios_load_user_font", 0x320);
	weak_label(context, "_bios_load_bios_font", 0x324);
	weak_label(context, "_bios_load_bios_font_default", 0x328);
	//TODO: more functions in the middle here
	weak_label(context, "_bios_prng_mod", 0x338);
	weak_label(context, "_bios_prng", 0x33C);
	weak_label(context, "_bios_clear_comm", 0x340);
	weak_label(context, "_bios_comm_update", 0x344);
	//TODO: more functions in the middle here
	weak_label(context, "_bios_sega_logo", 0x364);
	weak_label(context, "_bios_set_vint", 0x368);
	//TODO: more functions at the end here

	weak_label(context, "WORD_RAM", 0x200000);
	weak_label(context, "CD_RESET_IFL2", 0xA12000);
	weak_label(context, "CD_RESET_IFL2_BYTE", 0xA12001);
	weak_label(context, "CD_WRITE_PROTECT", 0xA12002);
	weak_label(context, "CD_MEM_MODE", 0xA12003);
	weak_label(context, "CDC_CTRL", 0xA12004);
	weak_label(context, "HINT_VECTOR", 0xA12006);
	weak_label(context, "CDC_HOST_DATA", 0xA12008);
	weak_label(context, "STOP_WATCH", 0xA1200C);
	weak_label(context, "COMM_MAIN_FLAG", 0xA1200E);
	weak_label(context, "COMM_SUB_FLAG", 0xA1200F);
	weak_label(context, "COMM_CMD0", 0xA12010);
	weak_label(context, "COMM_CMD1", 0xA12012);
	weak_label(context, "COMM_CMD2", 0xA12014);
	weak_label(context, "COMM_CMD3", 0xA12016);
	weak_label(context, "COMM_CMD4", 0xA12018);
	weak_label(context, "COMM_CMD5", 0xA1201A);
	weak_label(context, "COMM_CMD6", 0xA1201C);
	weak_label(context, "COMM_CMD7", 0xA1201E);
	weak_label(context, "COMM_STATUS0", 0xA12020);
	weak_label(context, "COMM_STATUS1", 0xA12022);
	weak_label(context, "COMM_STATUS2", 0xA12024);
	weak_label(context, "COMM_STATUS3", 0xA12026);
	weak_label(context, "COMM_STATUS4", 0xA12028);
	weak_label(context, "COMM_STATUS5", 0xA1202A);
	weak_label(context, "COMM_STATUS6", 0xA1202C);
	weak_label(context, "COMM_STATUS7", 0xA1202E);
}

void add_segacd_subcpu_labels(disasm_context *context)
{
	weak_label(context, "bios_common_work", 0x5E80);
	weak_label(context, "_setjmptbl", 0x5F0A);
	weak_label(context, "_waitvsync", 0x5F10);
	weak_label(context, "_buram", 0x5F16);
	weak_label(context, "_cdboot", 0x5F1C);
	weak_label(context, "_cdbios", 0x5F22);
	weak_label(context, "_usercall0", 0x5F28);
	weak_label(context, "_usercall1", 0x5F2E);
	weak_label(context, "_usercall2", 0x5F34);
	weak_label(context, "_usercall2Address", 0x5F36);
	weak_label(context, "_usercall3", 0x5F3A);
	weak_label(context, "_adrerr", 0x5F40);
	weak_label(context, "_adrerrAddress", 0x5F42);
	weak_label(context, "_coderr", 0x5F46);
	weak_label(context, "_coderrAddress", 0x5F48);
	weak_label(context, "_diverr", 0x5F4C);
	weak_label(context, "_diverrAddress", 0x5F4E);
	weak_label(context, "_chkerr", 0x5F52);
	weak_label(context, "_chkerrAddress", 0x5F54);
	weak_label(context, "_trperr", 0x5F58);
	weak_label(context, "_trperrAddress", 0x5F5A);
	weak_label(context, "_spverr", 0x5F5E);
	weak_label(context, "_spverrAddress", 0x5F60);
	weak_label(context, "_trace", 0x5F64);
	weak_label(context, "_traceAddress", 0x5F66);
	weak_label(context, "_nocod0", 0x5F6A);
	weak_label(context, "_nocod0Address", 0x5F6C);
	weak_label(context, "_nocod0", 0x5F70);
	weak_label(context, "_nocod0Address", 0x5F72);
	weak_label(context, "_slevel1", 0x5F76);
	weak_label(context, "_slevel1Address", 0x5F78);
	weak_label(context, "_slevel2", 0x5F7C);
	weak_label(context, "_slevel2Address", 0x5F7E);
	weak_label(context, "_slevel3", 0x5F82);
	weak_label(context, "_slevel3Address", 0x5F84);
	weak_label(context, "WORD_RAM_2M", 0x80000);
	weak_label(context, "WORD_RAM_1M", 0xC0000);
	weak_label(context, "LED_CONTROL", 0xFFFF8000);
	weak_label(context, "VERSION_RESET", 0xFFFF8001);
	weak_label(context, "MEM_MODE_WORD", 0xFFFF8002);
	weak_label(context, "MEM_MODE_BYTE", 0xFFFF8003);
	weak_label(context, "CDC_CTRL", 0xFFFF8004);
	weak_label(context, "CDC_AR", 0xFFFF8005);
	weak_label(context, "CDC_REG_DATA_WORD", 0xFFFF8006);
	weak_label(context, "CDC_REG_DATA", 0xFFFF8007);
	weak_label(context, "CDC_HOST_DATA", 0xFFFF8008);
	weak_label(context, "CDC_DMA_ADDR", 0xFFFF800A);
	weak_label(context, "STOP_WATCH", 0xFFFF800C);
	weak_label(context, "COMM_MAIN_FLAG", 0xFFFF800E);
	weak_label(context, "COMM_SUB_FLAG", 0xFFFF800F);
	weak_label(context, "COMM_CMD0", 0xFFFF8010);
	weak_label(context, "COMM_CMD1", 0xFFFF8012);
	weak_label(context, "COMM_CMD2", 0xFFFF8014);
	weak_label(context, "COMM_CMD3", 0xFFFF8016);
	weak_label(context, "COMM_CMD4", 0xFFFF8018);
	weak_label(context, "COMM_CMD5", 0xFFFF801A);
	weak_label(context, "COMM_CMD6", 0xFFFF801C);
	weak_label(context, "COMM_CMD7", 0xFFFF801E);
	weak_label(context, "COMM_STATUS0", 0xFFFF8020);
	weak_label(context, "COMM_STATUS1", 0xFFFF8022);
	weak_label(context, "COMM_STATUS2", 0xFFFF8024);
	weak_label(context, "COMM_STATUS3", 0xFFFF8026);
	weak_label(context, "COMM_STATUS4", 0xFFFF8028);
	weak_label(context, "COMM_STATUS5", 0xFFFF802A);
	weak_label(context, "COMM_STATUS6", 0xFFFF802C);
	weak_label(context, "COMM_STATUS7", 0xFFFF802E);
	weak_label(context, "TIMER_WORD", 0xFFFF8030);
	weak_label(context, "TIMER", 0xFFFF8031);
	weak_label(context, "INT_MASK_WORD", 0xFFFF8032);
	weak_label(context, "INT_MASK", 0xFFFF8033);
	weak_label(context, "CDD_FADER", 0xFFFF8034);
	weak_label(context, "CDD_CTRL_WORD", 0xFFFF8036);
	weak_label(context, "CDD_CTRL_BYTE", 0xFFFF8037);
}

disasm_context *create_68000_disasm(void)
{
	disasm_context *context = calloc(1, sizeof(disasm_context));
	context->address_mask = 0xFFFFFF;
	context->invalid_inst_addr_mask = 1;
	context->visit_preshift = 1;
	return context;
}

