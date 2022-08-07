#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdint.h>
#include "m68k_core.h"
#ifdef NEW_CORE
#include "z80.h"
#else
#include "z80_to_x86.h"
#endif

typedef struct disp_def {
	struct disp_def * next;
	char *            param;
	uint32_t          index;
	char              format_char;
} disp_def;

typedef struct bp_def {
	struct bp_def *next;
	char          *commands;
	uint32_t      address;
	uint32_t      index;
} bp_def;

typedef struct debug_root debug_root;
typedef uint8_t (*resolver)(debug_root *root, const char *name, uint32_t *out);
typedef uint8_t (*reader)(debug_root *root, uint32_t *out, char size);

struct debug_root {
	void     *cpu_context;
	bp_def   *breakpoints;
	disp_def *displays;
	resolver resolve;
	reader   read_mem;
	uint32_t bp_index;
	uint32_t disp_index;
	uint32_t branch_t;
	uint32_t branch_f;
	uint32_t address;
};

debug_root *find_root(void *cpu);
bp_def ** find_breakpoint(bp_def ** cur, uint32_t address);
bp_def ** find_breakpoint_idx(bp_def ** cur, uint32_t index);
void add_display(disp_def ** head, uint32_t *index, char format_char, char * param);
void remove_display(disp_def ** head, uint32_t index);
void debugger(m68k_context * context, uint32_t address);
z80_context * zdebugger(z80_context * context, uint16_t address);
void print_m68k_help();
void print_z80_help();

#endif //DEBUG_H_
