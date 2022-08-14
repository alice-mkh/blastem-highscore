#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdint.h>
#include "tern.h"
#include "m68k_core.h"
#ifdef NEW_CORE
#include "z80.h"
#else
#include "z80_to_x86.h"
#endif

typedef enum {
	TOKEN_NONE,
	TOKEN_NUM,
	TOKEN_NAME,
	TOKEN_OPER,
	TOKEN_SIZE,
	TOKEN_LBRACKET,
	TOKEN_RBRACKET,
	TOKEN_LPAREN,
	TOKEN_RPAREN
} token_type;

typedef struct {
	token_type type;
	union {
		char     *str;
		char     op[3];
		uint32_t num;
	} v;
} token;

typedef enum {
	EXPR_NONE,
	EXPR_SCALAR,
	EXPR_UNARY,
	EXPR_BINARY,
	EXPR_SIZE,
	EXPR_MEM
} expr_type;

typedef struct expr expr;

struct expr {
	expr_type type;
	expr      *left;
	expr      *right;
	token     op;
};

typedef struct {
	char     *raw;
	expr     *parsed;
	uint32_t value;
} command_arg;

typedef struct debug_root debug_root;
typedef uint8_t (*raw_cmd)(debug_root *root, char *format, char *param);
typedef uint8_t (*cmd)(debug_root *root, char *format, int num_args, command_arg *args);

typedef struct {
	const char **names;
	const char *usage;
	const char *desc;
	raw_cmd    raw_impl;
	cmd        impl;
	int        min_args;
	int        max_args;
	uint8_t    skip_eval;
	uint8_t    visited;
} command_def;

typedef struct disp_def {
	struct disp_def *next;
	char             *format;
	int              num_args;
	command_arg      *args;
	uint32_t          index;
} disp_def;

typedef struct {
	command_def *def;
	char        *format;
	char        *raw;
	command_arg *args;
	int         num_args;
} parsed_command;

typedef struct bp_def {
	struct bp_def  *next;
	parsed_command *commands;
	uint32_t       num_commands;
	uint32_t       address;
	uint32_t       index;
} bp_def;

typedef uint8_t (*resolver)(debug_root *root, const char *name, uint32_t *out);
typedef uint8_t (*setter)(debug_root *root, const char *name, uint32_t value);
typedef uint8_t (*reader)(debug_root *root, uint32_t *out, char size);
typedef uint8_t (*writer)(debug_root *root, uint32_t address, uint32_t value, char size);

struct debug_root {
	void      *cpu_context;
	bp_def    *breakpoints;
	disp_def  *displays;
	tern_node *commands;
	resolver  resolve;
	reader    read_mem;
	setter    set;
	writer    write_mem;
	uint32_t  bp_index;
	uint32_t  disp_index;
	uint32_t  branch_t;
	uint32_t  branch_f;
	void      *inst;
	uint32_t  address;
	uint32_t  after;
};

debug_root *find_root(void *cpu);
debug_root *find_m68k_root(m68k_context *context);
debug_root *find_z80_root(z80_context *context);
bp_def ** find_breakpoint(bp_def ** cur, uint32_t address);
bp_def ** find_breakpoint_idx(bp_def ** cur, uint32_t index);
void add_display(disp_def ** head, uint32_t *index, char format_char, char * param);
void remove_display(disp_def ** head, uint32_t index);
void debugger(m68k_context * context, uint32_t address);
z80_context * zdebugger(z80_context * context, uint16_t address);
void print_m68k_help();
void print_z80_help();

#endif //DEBUG_H_
