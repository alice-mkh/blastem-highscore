#include "debug.h"
#include "genesis.h"
#include "68kinst.h"
#include "segacd.h"
#include "blastem.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
#include "render.h"
#include "util.h"
#include "terminal.h"
#include "z80inst.h"

#ifdef NEW_CORE
#define Z80_OPTS opts
#else
#define Z80_OPTS options
#endif



static debug_root roots[5];
static uint32_t num_roots;
#define MAX_DEBUG_ROOTS (sizeof(roots)/sizeof(*roots))

debug_root *find_root(void *cpu)
{
	for (uint32_t i = 0; i < num_roots; i++)
	{
		if (roots[i].cpu_context == cpu) {
			return roots + i;
		}
	}
	if (num_roots < MAX_DEBUG_ROOTS) {
		num_roots++;
		memset(roots + num_roots - 1, 0, sizeof(debug_root));
		roots[num_roots-1].cpu_context = cpu;
		return roots + num_roots - 1;
	}
	return NULL;
}

bp_def ** find_breakpoint(bp_def ** cur, uint32_t address)
{
	while (*cur) {
		if ((*cur)->address == address) {
			break;
		}
		cur = &((*cur)->next);
	}
	return cur;
}

bp_def ** find_breakpoint_idx(bp_def ** cur, uint32_t index)
{
	while (*cur) {
		if ((*cur)->index == index) {
			break;
		}
		cur = &((*cur)->next);
	}
	return cur;
}

typedef enum {
	TOKEN_NONE,
	TOKEN_NUM,
	TOKEN_NAME,
	TOKEN_OPER,
	TOKEN_SIZE,
	TOKEN_LBRACKET,
	TOKEN_RBRACKET,
} token_type;

static const char *token_type_names[] = {
	"TOKEN_NONE",
	"TOKEN_NUM",
	"TOKEN_NAME",
	"TOKEN_OPER",
	"TOKEN_SIZE",
	"TOKEN_LBRACKET",
	"TOKEN_RBRACKET"
};

typedef struct {
	token_type type;
	union {
		char     *str;
		char     op[3];
		uint32_t num;
	} v;
} token;

static token parse_token(char *start, char **end)
{
	while(*start && isblank(*start) && *start != '\n')
	{
		++start;
	}
	if (!*start || *start == '\n') {
		return (token){
			.type = TOKEN_NONE
		};
		*end = start;
	}
	if (*start == '$' || (*start == '0' && start[1] == 'x')) {
		return (token) {
			.type = TOKEN_NUM,
			.v = {
				.num = strtol(start + (*start == '$' ? 1 : 2), end, 16)
			}
		};
	}
	if (isdigit(*start)) {
		return (token) {
			.type = TOKEN_NUM,
			.v = {
				.num = strtol(start, end, 10)
			}
		};
	}
	switch (*start)
	{
	case '+':
	case '-':
	case '*':
	case '/':
	case '&':
	case '|':
	case '^':
	case '~':
	case '=':
	case '!':
		if (*start == '!' && start[1] == '=') {
			*end = start + 2;
			return (token) {
				.type = TOKEN_OPER,
				.v = {
					.op = {*start, start[1], 0}
				}
			};
		}
		*end = start + 1;
		return (token) {
			.type = TOKEN_OPER,
			.v = {
				.op = {*start, 0}
			}
		};
	case '.':
		*end = start + 2;
		return (token) {
			.type = TOKEN_SIZE,
			.v = {
				.op = {start[1], 0}
			}
		};
	case '[':
		*end = start + 1;
		return (token) {
			.type = TOKEN_LBRACKET
		};
	case ']':
		*end = start + 1;
		return (token) {
			.type = TOKEN_RBRACKET
		};
	}
	*end = start + 1;
	while (**end && !isblank(**end) && **end != '.')
	{
		++*end;
	}
	char *name = malloc(*end - start + 1);
	memcpy(name, start, *end - start);
	name[*end-start] = 0;
	return (token) {
		.type = TOKEN_NAME,
		.v = {
			.str = name
		}
	};
}

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

static void free_expr(expr *e)
{
	if (!e) {
		return;
	}
	free_expr(e->left);
	free_expr(e->right);
	if (e->op.type == TOKEN_NAME) {
		free(e->op.v.str);
	}
	free(e);
}

static expr *parse_scalar_or_muldiv(char *start, char **end);
static expr *parse_expression(char *start, char **end);

static expr *parse_scalar(char *start, char **end)
{
	char *after_first;
	token first = parse_token(start, &after_first);
	if (!first.type) {
		return NULL;
	}
	if (first.type == TOKEN_SIZE) {
		fprintf(stderr, "Unexpected TOKEN_SIZE '.%s'\n", first.v.op);
		return NULL;
	}
	if (first.type == TOKEN_OPER) {
		expr *target = parse_scalar(after_first, end);
		if (!target) {
			fprintf(stderr, "Unary expression %s needs value\n", first.v.op);
			return NULL;
		}
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_UNARY;
		ret->op = first;
		ret->left = target;
		*end = after_first;
		return ret;
	}
	if (first.type == TOKEN_LBRACKET) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		ret->left = parse_expression(after_first, end);
		if (!ret->left) {
			fprintf(stderr, "Expression expected after `[`\n");
			free(ret);
			return NULL;
		}
		token rbrack = parse_token(*end, end);
		if (rbrack.type != TOKEN_RBRACKET) {
			fprintf(stderr, "Missing closing `]`");
			free_expr(ret);
			return NULL;
		}
		char *after_size;
		token size = parse_token(*end, &after_size);
		if (size.type == TOKEN_SIZE) {
			*end = after_size;
			ret->op = size;
		}
		return ret;
	}
	token second = parse_token(after_first, end);
	if (second.type != TOKEN_SIZE) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		*end = after_first;
		return ret;
	}
	expr *ret = calloc(1, sizeof(expr));
	ret->type = EXPR_SIZE;
	ret->left = calloc(1, sizeof(expr));
	ret->left->type = EXPR_SCALAR;
	ret->left->op = second;
	ret->op = first;
	return ret;
}

static expr *maybe_binary(expr *left, char *start, char **end)
{
	char *after_first;
	token first = parse_token(start, &after_first);
	if (first.type != TOKEN_OPER) {
		*end = start;
		return left;
	}
	expr *bin = calloc(1, sizeof(expr));
	bin->left = left;
	bin->op = first;
	bin->type = EXPR_BINARY;
	switch (first.v.op[0])
	{
	case '*':
	case '/':
	case '&':
	case '|':
	case '^':
		bin->right = parse_scalar(after_first, end);
		return maybe_binary(bin, *end, end);
	case '+':
	case '-':
		bin->right = parse_scalar_or_muldiv(after_first, end);
		return maybe_binary(bin, *end, end);
	case '=':
	case '!':
		bin->right = parse_expression(after_first, end);
		return bin;
	default:
		bin->left = NULL;
		free(bin);
		return left;
	}
}

static expr *maybe_muldiv(expr *left, char *start, char **end)
{
	char *after_first;
	token first = parse_token(start, &after_first);
	if (first.type != TOKEN_OPER) {
		*end = start;
		return left;
	}
	expr *bin = calloc(1, sizeof(expr));
	bin->left = left;
	bin->op = first;
	bin->type = EXPR_BINARY;
	switch (first.v.op[0])
	{
	case '*':
	case '/':
	case '&':
	case '|':
	case '^':
		bin->right = parse_scalar(after_first, end);
		return maybe_binary(bin, *end, end);
	default:
		bin->left = NULL;
		free(bin);
		return left;
	}
}

static expr *parse_scalar_or_muldiv(char *start, char **end)
{
	char *after_first;
	token first = parse_token(start, &after_first);
	if (!first.type) {
		return NULL;
	}
	if (first.type == TOKEN_SIZE) {
		fprintf(stderr, "Unexpected TOKEN_SIZE '.%s'\n", first.v.op);
		return NULL;
	}
	if (first.type == TOKEN_OPER) {
		expr *target = parse_scalar(after_first, end);
		if (!target) {
			fprintf(stderr, "Unary expression %s needs value\n", first.v.op);
			return NULL;
		}
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_UNARY;
		ret->op = first;
		ret->left = target;
		return ret;
	}
	if (first.type == TOKEN_LBRACKET) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		ret->left = parse_expression(after_first, end);
		if (!ret->left) {
			fprintf(stderr, "Expression expected after `[`\n");
			free(ret);
			return NULL;
		}
		token rbrack = parse_token(*end, end);
		if (rbrack.type != TOKEN_RBRACKET) {
			fprintf(stderr, "Missing closing `]`");
			free_expr(ret);
			return NULL;
		}
		char *after_size;
		token size = parse_token(*end, &after_size);
		if (size.type == TOKEN_SIZE) {
			*end = after_size;
			ret->op = size;
		}
		return ret;
	}
	char *after_second;
	token second = parse_token(after_first, &after_second);
	if (second.type == TOKEN_OPER) {
		expr *ret;
		expr *bin = calloc(1, sizeof(expr));
		bin->type = EXPR_BINARY;
		bin->left = calloc(1, sizeof(expr));
		bin->left->type = EXPR_SCALAR;
		bin->left->op = first;
		bin->op = second;
		switch (second.v.op[0])
		{
		case '*':
		case '/':
		case '&':
		case '|':
		case '^':
			bin->right = parse_scalar(after_second, end);
			return maybe_muldiv(bin, *end, end);
		case '+':
		case '-':
		case '=':
		case '!':
			ret = bin->left;
			bin->left = NULL;
			free_expr(bin);
			return ret;
		default:
			fprintf(stderr, "%s is not a valid binary operator\n", second.v.op);
			free(bin->left);
			free(bin);
			return NULL;
		}
	} else if (second.type == TOKEN_SIZE) {
		expr *value = calloc(1, sizeof(expr));
		value->type = EXPR_SIZE;
		value->op = second;
		value->left = calloc(1, sizeof(expr));
		value->left->type = EXPR_SCALAR;
		value->left->op = first;
		return maybe_muldiv(value, after_second, end);
	} else {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		*end = after_first;
		return ret;
	}
}

static expr *parse_expression(char *start, char **end)
{
	char *after_first;
	token first = parse_token(start, &after_first);
	if (!first.type) {
		return NULL;
	}
	if (first.type == TOKEN_SIZE) {
		fprintf(stderr, "Unexpected TOKEN_SIZE '.%s'\n", first.v.op);
		return NULL;
	}
	if (first.type == TOKEN_OPER) {
		expr *target = parse_scalar(after_first, end);
		if (!target) {
			fprintf(stderr, "Unary expression %s needs value\n", first.v.op);
			return NULL;
		}
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_UNARY;
		ret->op = first;
		ret->left = target;
		return ret;
	}
	if (first.type == TOKEN_LBRACKET) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		ret->left = parse_expression(after_first, end);
		if (!ret->left) {
			fprintf(stderr, "Expression expected after `[`\n");
			free(ret);
			return NULL;
		}
		token rbrack = parse_token(*end, end);
		if (rbrack.type != TOKEN_RBRACKET) {
			fprintf(stderr, "Missing closing `]`");
			free_expr(ret);
			return NULL;
		}
		char *after_size;
		token size = parse_token(*end, &after_size);
		if (size.type == TOKEN_SIZE) {
			*end = after_size;
			ret->op = size;
		}
		return ret;
	}
	char *after_second;
	token second = parse_token(after_first, &after_second);
	if (second.type == TOKEN_OPER) {
		expr *bin = calloc(1, sizeof(expr));
		bin->type = EXPR_BINARY;
		bin->left = calloc(1, sizeof(expr));
		bin->left->type = EXPR_SCALAR;
		bin->left->op = first;
		bin->op = second;
		switch (second.v.op[0])
		{
		case '*':
		case '/':
		case '&':
		case '|':
		case '^':
			bin->right = parse_scalar(after_second, end);
			return maybe_binary(bin, *end, end);
		case '+':
		case '-':
			bin->right = parse_scalar_or_muldiv(after_second, end);
			return maybe_binary(bin, *end, end);
		case '=':
		case '!':
			bin->right = parse_expression(after_second, end);
			return bin;
		default:
			fprintf(stderr, "%s is not a valid binary operator\n", second.v.op);
			free(bin->left);
			free(bin);
			return NULL;
		}
	} else if (second.type == TOKEN_SIZE) {
		expr *value = calloc(1, sizeof(expr));
		value->type = EXPR_SIZE;
		value->op = second;
		value->left = calloc(1, sizeof(expr));
		value->left->type = EXPR_SCALAR;
		value->left->op = first;
		return maybe_binary(value, after_second, end);
	} else {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		*end = after_first;
		return ret;
	}
}

typedef struct debug_context debug_context;
typedef uint8_t (*resolver)(debug_context *context, const char *name, uint32_t *out);
typedef uint8_t (*reader)(debug_context *context, uint32_t *out, char size);

struct debug_context {
	resolver resolve;
	reader   read_mem;
	void     *system;
};

uint8_t eval_expr(debug_context *context, expr *e, uint32_t *out)
{
	uint32_t right;
	switch(e->type)
	{
	case EXPR_SCALAR:
		if (e->op.type == TOKEN_NAME) {
			return context->resolve(context, e->op.v.str, out);
		} else {
			*out = e->op.v.num;
			return 1;
		}
	case EXPR_UNARY:
		if (!eval_expr(context, e->left, out)) {
			return 0;
		}
		switch (e->op.v.op[0])
		{
		case '!':
			*out = !*out;
			break;
		case '~':
			*out = ~*out;
			break;
		case '-':
			*out = -*out;
			break;
		default:
			return 0;
		}
		return 1;
	case EXPR_BINARY:
		if (!eval_expr(context, e->left, out) || !eval_expr(context, e->right, &right)) {
			return 0;
		}
		switch (e->op.v.op[0])
		{
		case '+':
			*out += right;
			break;
		case '-':
			*out -= right;
			break;
		case '*':
			*out *= right;
			break;
		case '/':
			*out /= right;
			break;
		case '&':
			*out &= right;
			break;
		case '|':
			*out |= right;
			break;
		case '^':
			*out ^= right;
			break;
		case '=':
			*out = *out == right;
			break;
		case '!':
			*out = *out != right;
			break;
		default:
			return 0;
		}
		return 1;
	case EXPR_SIZE:
		if (!eval_expr(context, e->left, out)) {
			return 0;
		}
		switch (e->op.v.op[0])
		{
		case 'b':
			*out &= 0xFF;
			break;
		case 'w':
			*out &= 0xFFFF;
			break;
		}
		return 1;
	case EXPR_MEM:
		if (!eval_expr(context, e->left, out)) {
			return 0;
		}
		return context->read_mem(context, out, e->op.v.op[0]);
	default:
		return 0;
	}
}

void add_display(disp_def ** head, uint32_t *index, char format_char, char * param)
{
	disp_def * ndisp = malloc(sizeof(*ndisp));
	ndisp->format_char = format_char;
	ndisp->param = strdup(param);
	ndisp->next = *head;
	ndisp->index = *index++;
	*head = ndisp;
}

void remove_display(disp_def ** head, uint32_t index)
{
	while (*head) {
		if ((*head)->index == index) {
			disp_def * del_disp = *head;
			*head = del_disp->next;
			free(del_disp->param);
			free(del_disp);
		} else {
			head = &(*head)->next;
		}
	}
}

char * find_param(char * buf)
{
	for (; *buf; buf++) {
		if (*buf == ' ') {
			if (*(buf+1)) {
				return buf+1;
			}
		}
	}
	return NULL;
}

void strip_nl(char * buf)
{
	for(; *buf; buf++) {
		if (*buf == '\n') {
			*buf = 0;
			return;
		}
	}
}

static uint8_t m68k_read_byte(uint32_t address, m68k_context *context)
{
	//TODO: share this implementation with GDB debugger
	return read_byte(address, (void **)context->mem_pointers, &context->options->gen, context);
}

static uint16_t m68k_read_word(uint32_t address, m68k_context *context)
{
	return read_word(address, (void **)context->mem_pointers, &context->options->gen, context);
}

static uint32_t m68k_read_long(uint32_t address, m68k_context *context)
{
	return m68k_read_word(address, context) << 16 | m68k_read_word(address + 2, context);
}

static uint8_t read_m68k(m68k_context *context, uint32_t *out, char size)
{
	if (size == 'b') {
		*out = m68k_read_byte(*out, context);
	} else if (size == 'l') {
		*out = m68k_read_long(*out, context);
	} else {
		*out = m68k_read_word(*out, context);
	}
	return 1;
}

static uint8_t read_genesis(debug_context *context, uint32_t *out, char size)
{
	genesis_context *gen = context->system;
	return read_m68k(gen->m68k, out, size);
}

static uint8_t resolve_m68k(m68k_context *context, const char *name, uint32_t *out)
{
	if (name[0] == 'd' && name[1] >= '0' && name[1] <= '7') {
		*out = context->dregs[name[1]-'0'];
	} else if (name[0] == 'a' && name[1] >= '0' && name[1] <= '7') {
		*out = context->aregs[name[1]-'0'];
	} else if (name[0] == 's' && name[1] == 'r') {
		*out = context->status << 8;
		for (int flag = 0; flag < 5; flag++) {
			*out |= context->flags[flag] << (4-flag);
		}
	} else if(name[0] == 'c') {
		*out = context->current_cycle;
	} else if (name[0] == 'p' && name[1] == 'c') {
		//FIXME
		//*out = address;
	} else {
		return 0;
	}
	return 1;
}

static uint8_t resolve_genesis(debug_context *context, const char *name, uint32_t *out)
{
	genesis_context *gen = context->system;
	if (resolve_m68k(gen->m68k, name, out)) {
		return 1;
	}
	if (!strcmp(name, "f") || !strcmp(name, "frame")) {
		*out = gen->vdp->frame;
		return 1;
	}
	return 0;
}

void debugger_print(m68k_context *context, char format_char, char *param, uint32_t address)
{
	uint32_t value;
	char format[8];
	strcpy(format, "%s: %d\n");
	switch (format_char)
	{
	case 'x':
	case 'X':
	case 'd':
	case 'c':
	case 's':
		format[5] = format_char;
		break;
	case '\0':
		break;
	default:
		fprintf(stderr, "Unrecognized format character: %c\n", format_char);
	}
	debug_context c = {
		.resolve = resolve_genesis,
		.read_mem = read_genesis,
		.system = context->system
	};
	char *after;
	expr *e = parse_expression(param, &after);
	if (e) {
		if (!eval_expr(&c, e, &value)) {
			fprintf(stderr, "Failed to eval %s\n", param);
		}
		free_expr(e);
	} else {
		fprintf(stderr, "Failed to parse %s\n", param);
	}
	/*
	if (param[0] == 'd' && param[1] >= '0' && param[1] <= '7') {
		value = context->dregs[param[1]-'0'];
		if (param[2] == '.') {
			if (param[3] == 'w') {
				value &= 0xFFFF;
			} else if (param[3] == 'b') {
				value &= 0xFF;
			}
		}
	} else if (param[0] == 'a' && param[1] >= '0' && param[1] <= '7') {
		value = context->aregs[param[1]-'0'];
		if (param[2] == '.') {
			if (param[3] == 'w') {
				value &= 0xFFFF;
			} else if (param[3] == 'b') {
				value &= 0xFF;
			}
		}
	} else if (param[0] == 's' && param[1] == 'r') {
		value = (context->status << 8);
		for (int flag = 0; flag < 5; flag++) {
			value |= context->flags[flag] << (4-flag);
		}
	} else if(param[0] == 'c') {
		value = context->current_cycle;
	} else if(param[0] == 'f') {
		genesis_context *gen = context->system;
		value = gen->vdp->frame;
	} else if (param[0] == 'p' && param[1] == 'c') {
		value = address;
	} else if ((param[0] == '0' && param[1] == 'x') || param[0] == '$') {
		char *after;
		uint32_t p_addr = strtol(param+(param[0] == '0' ? 2 : 1), &after, 16);
		if (after[0] == '.' && after[1] == 'l') {
			value = m68k_read_long(p_addr, context);
		} else if (after[0] == '.' && after[1] == 'b') {
			value = m68k_read_byte(p_addr, context);
		} else {
			value = m68k_read_word(p_addr, context);
		}
	} else if(param[0] == '(' && (param[1] == 'a' || param[1] == 'd') && param[2] >= '0' && param[2] <= '7' && param[3] == ')') {
		uint8_t reg = param[2] - '0';
		uint32_t p_addr = param[1] == 'a' ? context->aregs[reg] : context->dregs[reg];
		if (param[4] == '.' && param[5] == 'l') {
			value = m68k_read_long(p_addr, context);
		} else if (param[4] == '.' && param[5] == 'b') {
			value = m68k_read_byte(p_addr, context);
		} else {
			value = m68k_read_word(p_addr, context);
		}
	} else {
		fprintf(stderr, "Unrecognized parameter to p: %s\n", param);
		return;
	}*/
	if (format_char == 's') {
		char tmp[128];
		int i;
		for (i = 0; i < sizeof(tmp)-1; i++, value++)
		{
			char c = m68k_read_byte(value, context);
			if (c < 0x20 || c > 0x7F) {
				break;
			}
			tmp[i] = c;
		}
		tmp[i] = 0;
		printf(format, param, tmp);
	} else {
		printf(format, param, value);
	}
}

#ifndef NO_Z80

void zdebugger_print(z80_context * context, char format_char, char * param)
{
	uint32_t value;
	char format[8];
	strcpy(format, "%s: %d\n");
	genesis_context *system = context->system;
	switch (format_char)
	{
	case 'x':
	case 'X':
	case 'd':
	case 'c':
		format[5] = format_char;
		break;
	case '\0':
		break;
	default:
		fprintf(stderr, "Unrecognized format character: %c\n", format_char);
	}
	switch (param[0])
	{
#ifndef NEW_CORE
	case 'a':
		if (param[1] == 'f') {
			if(param[2] == '\'') {
				value = context->alt_regs[Z80_A] << 8;
				value |= context->alt_flags[ZF_S] << 7;
				value |= context->alt_flags[ZF_Z] << 6;
				value |= context->alt_flags[ZF_H] << 4;
				value |= context->alt_flags[ZF_PV] << 2;
				value |= context->alt_flags[ZF_N] << 1;
				value |= context->alt_flags[ZF_C];
			} else {
				value = context->regs[Z80_A] << 8;
				value |= context->flags[ZF_S] << 7;
				value |= context->flags[ZF_Z] << 6;
				value |= context->flags[ZF_H] << 4;
				value |= context->flags[ZF_PV] << 2;
				value |= context->flags[ZF_N] << 1;
				value |= context->flags[ZF_C];
			}
		} else if(param[1] == '\'') {
			value = context->alt_regs[Z80_A];
		} else {
			value = context->regs[Z80_A];
		}
		break;
	case 'b':
		if (param[1] == 'c') {
			if(param[2] == '\'') {
				value = context->alt_regs[Z80_B] << 8;
				value |= context->alt_regs[Z80_C];
			} else {
				value = context->regs[Z80_B] << 8;
				value |= context->regs[Z80_C];
			}
		} else if(param[1] == '\'') {
			value = context->alt_regs[Z80_B];
		} else if(param[1] == 'a') {
			value = context->bank_reg << 15;
		} else {
			value = context->regs[Z80_B];
		}
		break;
	case 'c':
		if(param[1] == '\'') {
			value = context->alt_regs[Z80_C];
		} else if(param[1] == 'y') {
			value = context->current_cycle;
		} else {
			value = context->regs[Z80_C];
		}
		break;
	case 'd':
		if (param[1] == 'e') {
			if(param[2] == '\'') {
				value = context->alt_regs[Z80_D] << 8;
				value |= context->alt_regs[Z80_E];
			} else {
				value = context->regs[Z80_D] << 8;
				value |= context->regs[Z80_E];
			}
		} else if(param[1] == '\'') {
			value = context->alt_regs[Z80_D];
		} else {
			value = context->regs[Z80_D];
		}
		break;
	case 'e':
		if(param[1] == '\'') {
			value = context->alt_regs[Z80_E];
		} else {
			value = context->regs[Z80_E];
		}
		break;
	case 'f':
		if(param[2] == '\'') {
			value = context->alt_flags[ZF_S] << 7;
			value |= context->alt_flags[ZF_Z] << 6;
			value |= context->alt_flags[ZF_H] << 4;
			value |= context->alt_flags[ZF_PV] << 2;
			value |= context->alt_flags[ZF_N] << 1;
			value |= context->alt_flags[ZF_C];
		} else {
			value = context->flags[ZF_S] << 7;
			value |= context->flags[ZF_Z] << 6;
			value |= context->flags[ZF_H] << 4;
			value |= context->flags[ZF_PV] << 2;
			value |= context->flags[ZF_N] << 1;
			value |= context->flags[ZF_C];
		}
		break;
	case 'h':
		if (param[1] == 'l') {
			if(param[2] == '\'') {
				value = context->alt_regs[Z80_H] << 8;
				value |= context->alt_regs[Z80_L];
			} else {
				value = context->regs[Z80_H] << 8;
				value |= context->regs[Z80_L];
			}
		} else if(param[1] == '\'') {
			value = context->alt_regs[Z80_H];
		} else {
			value = context->regs[Z80_H];
		}
		break;
	case 'l':
		if(param[1] == '\'') {
			value = context->alt_regs[Z80_L];
		} else {
			value = context->regs[Z80_L];
		}
		break;
	case 'i':
		if(param[1] == 'x') {
			if (param[2] == 'h') {
				value = context->regs[Z80_IXH];
			} else if(param[2] == 'l') {
				value = context->regs[Z80_IXL];
			} else {
				value = context->regs[Z80_IXH] << 8;
				value |= context->regs[Z80_IXL];
			}
		} else if(param[1] == 'y') {
			if (param[2] == 'h') {
				value = context->regs[Z80_IYH];
			} else if(param[2] == 'l') {
				value = context->regs[Z80_IYL];
			} else {
				value = context->regs[Z80_IYH] << 8;
				value |= context->regs[Z80_IYL];
			}
		} else if(param[1] == 'n') {
			value = context->int_cycle;
		} else if(param[1] == 'f' && param[2] == 'f' && param[3] == '1') {
			value = context->iff1;
		} else if(param[1] == 'f' && param[2] == 'f' && param[3] == '2') {
			value = context->iff2;
		} else {
			value = context->im;
		}
		break;
#endif
	case 's':
		if (param[1] == 'p') {
			value = context->sp;
		}
		break;
	case '0':
		if (param[1] == 'x') {
			uint16_t p_addr = strtol(param+2, NULL, 16);
			value = read_byte(p_addr, (void **)context->mem_pointers, &context->options->gen, context);
		}
		break;
	}
	printf(format, param, value);
}

z80_context * zdebugger(z80_context * context, uint16_t address)
{
	static char last_cmd[1024];
	char input_buf[1024];
	z80inst inst;
	genesis_context *system = context->system;
	init_terminal();
	//Check if this is a user set breakpoint, or just a temporary one
	debug_root *root = find_root(context);
	if (!root) {
		return context;
	}
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address);
	if (*this_bp) {
		printf("Z80 Breakpoint %d hit\n", (*this_bp)->index);
	} else {
		zremove_breakpoint(context, address);
	}
	uint8_t * pc = get_native_pointer(address, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
	if (!pc) {
		fatal_error("Failed to get native pointer on entering Z80 debugger at address %X\n", address);
	}
	for (disp_def * cur = root->displays; cur; cur = cur->next) {
		zdebugger_print(context, cur->format_char, cur->param);
	}
	uint8_t * after_pc = z80_decode(pc, &inst);
	z80_disasm(&inst, input_buf, address);
	printf("%X:\t%s\n", address, input_buf);
	uint16_t after = address + (after_pc-pc);
	int debugging = 1;
	while(debugging) {
		fputs(">", stdout);
		if (!fgets(input_buf, sizeof(input_buf), stdin)) {
			fputs("fgets failed", stderr);
			break;
		}
		strip_nl(input_buf);
		//hitting enter repeats last command
		if (input_buf[0]) {
			strcpy(last_cmd, input_buf);
		} else {
			strcpy(input_buf, last_cmd);
		}
		char * param;
		char format[8];
		uint32_t value;
		bp_def * new_bp;
		switch(input_buf[0])
		{
			case 'a':
				param = find_param(input_buf);
				if (!param) {
					fputs("a command requires a parameter\n", stderr);
					break;
				}
				value = strtol(param, NULL, 16);
				zinsert_breakpoint(context, value, (uint8_t *)zdebugger);
				debugging = 0;
				break;
			case 'b':
				param = find_param(input_buf);
				if (!param) {
					fputs("b command requires a parameter\n", stderr);
					break;
				}
				value = strtol(param, NULL, 16);
				zinsert_breakpoint(context, value, (uint8_t *)zdebugger);
				new_bp = malloc(sizeof(bp_def));
				new_bp->next = root->breakpoints;
				new_bp->address = value;
				new_bp->index = root->bp_index++;
				new_bp->commands = NULL;
				root->breakpoints = new_bp;
				printf("Z80 Breakpoint %d set at %X\n", new_bp->index, value);
				break;
			case 'c':
				puts("Continuing");
				debugging = 0;
				break;
			case 'd':
				if (input_buf[1] == 'i') {
					char format_char = 0;
					for(int i = 2; input_buf[i] != 0 && input_buf[i] != ' '; i++) {
						if (input_buf[i] == '/') {
							format_char = input_buf[i+1];
							break;
						}
					}
					param = find_param(input_buf);
					if (!param) {
						fputs("display command requires a parameter\n", stderr);
						break;
					}
					zdebugger_print(context, format_char, param);
					add_display(&root->displays, &root->disp_index, format_char, param);
				} else if (input_buf[1] == 'e' || input_buf[1] == ' ') {
					param = find_param(input_buf);
					if (!param) {
						fputs("delete command requires a parameter\n", stderr);
						break;
					}
					if (param[0] >= '0' && param[0] <= '9') {
						value = atoi(param);
						this_bp = find_breakpoint_idx(&root->breakpoints, value);
						if (!*this_bp) {
							fprintf(stderr, "Breakpoint %d does not exist\n", value);
							break;
						}
						new_bp = *this_bp;
						zremove_breakpoint(context, new_bp->address);
						*this_bp = new_bp->next;
						free(new_bp);
					} else if (param[0] == 'd') {
						param = find_param(param);
						if (!param) {
							fputs("delete display command requires a parameter\n", stderr);
							break;
						}
						remove_display(&root->displays, atoi(param));
					}
				}
				break;
			case 'n':
				//TODO: Handle conditional branch instructions
				if (inst.op == Z80_JP) {
					if (inst.addr_mode == Z80_IMMED) {
						after = inst.immed;
					} else if (inst.ea_reg == Z80_HL) {
#ifndef NEW_CORE
						after = context->regs[Z80_H] << 8 | context->regs[Z80_L];
					} else if (inst.ea_reg == Z80_IX) {
						after = context->regs[Z80_IXH] << 8 | context->regs[Z80_IXL];
					} else if (inst.ea_reg == Z80_IY) {
						after = context->regs[Z80_IYH] << 8 | context->regs[Z80_IYL];
#endif
					}
				} else if(inst.op == Z80_JR) {
					after += inst.immed;
				} else if(inst.op == Z80_RET) {
					uint8_t *sp = get_native_pointer(context->sp, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
					if (sp) {
						after = *sp;
						sp = get_native_pointer((context->sp + 1) & 0xFFFF, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
						if (sp) {
							after |= *sp << 8;
						}
					}
				}
				zinsert_breakpoint(context, after, (uint8_t *)zdebugger);
				debugging = 0;
				break;
			case 'p':
				param = find_param(input_buf);
				if (!param) {
					fputs("p command requires a parameter\n", stderr);
					break;
				}
				zdebugger_print(context, input_buf[1] == '/' ? input_buf[2] : 0, param);
				break;
			case 'q':
				puts("Quitting");
				exit(0);
				break;
			case 's': {
				param = find_param(input_buf);
				if (!param) {
					fputs("s command requires a file name\n", stderr);
					break;
				}
				memmap_chunk const *ram_chunk = NULL;
				for (int i = 0; i < context->Z80_OPTS->gen.memmap_chunks; i++)
				{
					memmap_chunk const *cur = context->Z80_OPTS->gen.memmap + i;
					if (cur->flags & MMAP_WRITE) {
						ram_chunk = cur;
						break;
					}
				}
				if (ram_chunk) {
					uint32_t size = ram_chunk->end - ram_chunk->start;
					if (size > ram_chunk->mask) {
						size = ram_chunk->mask+1;
					}
					uint8_t *buf = get_native_pointer(ram_chunk->start, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
					FILE * f = fopen(param, "wb");
					if (f) {
						if(fwrite(buf, 1, size, f) != size) {
							fputs("Error writing file\n", stderr);
						}
						fclose(f);
						printf("Wrote %d bytes to %s\n", size, param);
					} else {
						fprintf(stderr, "Could not open %s for writing\n", param);
					}
				} else {
					fputs("Failed to find a RAM memory chunk\n", stderr);
				}
				break;
			}
			case '?':
				print_z80_help();
				break;
			default:
				if (
					!context->Z80_OPTS->gen.debug_cmd_handler
					|| !context->Z80_OPTS->gen.debug_cmd_handler(&system->header, input_buf)
				) {
					fprintf(stderr, "Unrecognized debugger command %s\nUse '?' for help.\n", input_buf);
				}
				break;
		}
	}
	return context;
}

#endif

int run_debugger_command(m68k_context *context, uint32_t address, char *input_buf, m68kinst inst, uint32_t after);
int run_genesis_debugger_command(m68k_context *context, uint32_t address, char *input_buf)
{
	genesis_context * gen = context->system;
	char *param;
	uint32_t value;
	bp_def *new_bp;
	switch (input_buf[0])
	{
	case 'v':
		//VDP debug commands
		switch(input_buf[1])
		{
		case 's':
			vdp_print_sprite_table(gen->vdp);
			break;
		case 'r':
			vdp_print_reg_explain(gen->vdp);
			break;
		}
		break;
	case 'y':
		//YM-2612 debug commands
		switch(input_buf[1])
		{
		case 'c':
			if (input_buf[2] == ' ') {
				int channel = atoi(input_buf+3)-1;
				ym_print_channel_info(gen->ym, channel);
			} else {
				for (int i = 0; i < 6; i++) {
					ym_print_channel_info(gen->ym, i);
				}
			}
			break;
		case 't':
			ym_print_timer_info(gen->ym);
			break;
		}
		break;
	case 'u':
		if (gen->expansion) {
			segacd_context *cd = gen->expansion;
			if (input_buf[1]) {
				//TODO: filter out commands that are unsafe to run when we don't have the current Sub CPU address
				run_debugger_command(cd->m68k, 0, input_buf + 1, (m68kinst){}, 0);
			} else {
				cd->enter_debugger = 1;
				return 0;
			}
		} else {
			fputs("u command only valid when Sega/Mega CD is active\n", stderr);
		}
		break;
#ifndef NO_Z80
	case 'z':
		//Z80 debug commands
		switch(input_buf[1])
		{
		case 'b': {
			param = find_param(input_buf);
			if (!param) {
				fputs("zb command requires a parameter\n", stderr);
				break;
			}
			value = strtol(param, NULL, 16);
			debug_root *zroot = find_root(gen->z80);
			zinsert_breakpoint(gen->z80, value, (uint8_t *)zdebugger);
			new_bp = malloc(sizeof(bp_def));
			new_bp->next = zroot->breakpoints;
			new_bp->address = value;
			new_bp->index = zroot->bp_index++;
			zroot->breakpoints = new_bp;
			printf("Z80 Breakpoint %d set at %X\n", new_bp->index, value);
			break;
		}
		case 'p':
			param = find_param(input_buf);
			if (!param) {
				fputs("zp command requires a parameter\n", stderr);
				break;
			}
			zdebugger_print(gen->z80, input_buf[2] == '/' ? input_buf[3] : 0, param);
		}
		break;
#endif
	default:
		fprintf(stderr, "Unrecognized debugger command %s\nUse '?' for help.\n", input_buf);
		break;
	}
	return 1;
}

int run_subcpu_debugger_command(m68k_context *context, uint32_t address, char *input_buf)
{
	segacd_context *cd = context->system;
	switch (input_buf[0])
	{
	case 'm':
		if (input_buf[1]) {
			//TODO: filter out commands that are unsafe to run when we don't have the current Main CPU address
			return run_debugger_command(cd->genesis->m68k, 0, input_buf + 1, (m68kinst){}, 0);
		} else {
			cd->genesis->header.enter_debugger = 1;
			return 0;
		}
		break;
	default:
		fprintf(stderr, "Unrecognized debugger command %s\nUse '?' for help.\n", input_buf);
		break;
	}
	return 1;
}

int run_debugger_command(m68k_context *context, uint32_t address, char *input_buf, m68kinst inst, uint32_t after)
{
	char * param;
	char format_char;
	genesis_context *system = context->system;
	uint32_t value;
	bp_def *new_bp, **this_bp;
	debug_root *root = find_root(context);
	if (!root) {
		return 0;
	}
	switch(input_buf[0])
	{
		case 'c':
			if (input_buf[1] == 0 || input_buf[1] == 'o' && input_buf[2] == 'n')
			{
				puts("Continuing");
				return 0;
			} else if (input_buf[1] == 'o' && input_buf[2] == 'm') {
				param = find_param(input_buf);
				if (!param) {
					fputs("com command requires a parameter\n", stderr);
					break;
				}
				bp_def **target = find_breakpoint_idx(&root->breakpoints, atoi(param));
				if (!target) {
					fprintf(stderr, "Breakpoint %s does not exist!\n", param);
					break;
				}
				printf("Enter commands for breakpoing %d, type end when done\n", atoi(param));
				char cmd_buf[1024];
				char *commands = NULL;
				for (;;)
				{
					fputs(">>", stdout);
					fflush(stdout);
					fgets(cmd_buf, sizeof(cmd_buf), stdin);
					if (strcmp(cmd_buf, "end\n")) {
						if (commands) {
							char *tmp = commands;
							commands = alloc_concat(commands, cmd_buf);
							free(tmp);
						} else {
							commands = strdup(cmd_buf);
						}
					} else {
						break;
					}
				}
				(*target)->commands = commands;
			} else {
			}
			break;
		case 'b':
			if (input_buf[1] == 't') {
				uint32_t stack = context->aregs[7];
				uint8_t non_adr_count = 0;
				do {
					uint32_t bt_address = m68k_instruction_fetch(stack, context);
					bt_address = get_instruction_start(context->options, bt_address - 2);
					if (bt_address) {
						stack += 4;
						non_adr_count = 0;
						m68k_decode(m68k_instruction_fetch, context, &inst, bt_address);
						m68k_disasm(&inst, input_buf);
						printf("%X: %s\n", bt_address, input_buf);
					} else {
						//non-return address value on stack can be word wide
						stack += 2;
						non_adr_count++;
					}
					//TODO: Make sure we don't wander into an invalid memory region
				} while (stack && non_adr_count < 6);
			} else {
				param = find_param(input_buf);
				if (!param) {
					fputs("b command requires a parameter\n", stderr);
					break;
				}
				value = strtol(param, NULL, 16);
				insert_breakpoint(context, value, debugger);
				new_bp = malloc(sizeof(bp_def));
				new_bp->next = root->breakpoints;
				new_bp->address = value;
				new_bp->index = root->bp_index++;
				new_bp->commands = NULL;
				root->breakpoints = new_bp;
				printf("68K Breakpoint %d set at %X\n", new_bp->index, value);
			}
			break;
		case 'a':
			param = find_param(input_buf);
			if (!param) {
				fputs("a command requires a parameter\n", stderr);
				break;
			}
			value = strtol(param, NULL, 16);
			insert_breakpoint(context, value, debugger);
			return 0;
		case 'd':
			if (input_buf[1] == 'i') {
				format_char = 0;
				for(int i = 2; input_buf[i] != 0 && input_buf[i] != ' '; i++) {
					if (input_buf[i] == '/') {
						format_char = input_buf[i+1];
						break;
					}
				}
				param = find_param(input_buf);
				if (!param) {
					fputs("display command requires a parameter\n", stderr);
					break;
				}
				debugger_print(context, format_char, param, address);
				add_display(&root->displays, &root->disp_index, format_char, param);
			} else {
				param = find_param(input_buf);
				if (!param) {
					fputs("d command requires a parameter\n", stderr);
					break;
				}
				value = atoi(param);
				this_bp = find_breakpoint_idx(&root->breakpoints, value);
				if (!*this_bp) {
					fprintf(stderr, "Breakpoint %d does not exist\n", value);
					break;
				}
				new_bp = *this_bp;
				*this_bp = (*this_bp)->next;
				if (new_bp->commands) {
					free(new_bp->commands);
				}
				free(new_bp);
			}
			break;
		case 'p':
			format_char = 0;
			for(int i = 1; input_buf[i] != 0 && input_buf[i] != ' '; i++) {
				if (input_buf[i] == '/') {
					format_char = input_buf[i+1];
					break;
				}
			}
			param = find_param(input_buf);
			if (param) {
				debugger_print(context, format_char, param, address);
			} else {
				m68k_disasm(&inst, input_buf);
				printf("%X: %s\n", address, input_buf);
			}

			break;
		case 'n':
			if (inst.op == M68K_RTS) {
				after = m68k_read_long(context->aregs[7], context);
			} else if (inst.op == M68K_RTE || inst.op == M68K_RTR) {
				after = m68k_read_long(context->aregs[7] + 2, context);
			} else if(m68k_is_noncall_branch(&inst)) {
				if (inst.op == M68K_BCC && inst.extra.cond != COND_TRUE) {
					root->branch_f = after;
					root->branch_t = m68k_branch_target(&inst, context->dregs, context->aregs);
					insert_breakpoint(context, root->branch_t, debugger);
				} else if(inst.op == M68K_DBCC) {
					if ( inst.extra.cond == COND_FALSE) {
						if (context->dregs[inst.dst.params.regs.pri] & 0xFFFF) {
							after = m68k_branch_target(&inst, context->dregs, context->aregs);
						}
					} else {
						root->branch_t = after;
						root->branch_f = m68k_branch_target(&inst, context->dregs, context->aregs);
						insert_breakpoint(context, root->branch_f, debugger);
					}
				} else {
					after = m68k_branch_target(&inst, context->dregs, context->aregs);
				}
			}
			insert_breakpoint(context, after, debugger);
			return 0;
		case 'o':
			if (inst.op == M68K_RTS) {
				after = m68k_read_long(context->aregs[7], context);
			} else if (inst.op == M68K_RTE || inst.op == M68K_RTR) {
				after = m68k_read_long(context->aregs[7] + 2, context);
			} else if(m68k_is_noncall_branch(&inst)) {
				if (inst.op == M68K_BCC && inst.extra.cond != COND_TRUE) {
					root->branch_t = m68k_branch_target(&inst, context->dregs, context->aregs)  & 0xFFFFFF;
					if (root->branch_t < after) {
							root->branch_t = 0;
					} else {
						root->branch_f = after;
						insert_breakpoint(context, root->branch_t, debugger);
					}
				} else if(inst.op == M68K_DBCC) {
					uint32_t target = m68k_branch_target(&inst, context->dregs, context->aregs)  & 0xFFFFFF;
					if (target > after) {
						if (inst.extra.cond == COND_FALSE) {
							after = target;
						} else {
							root->branch_f = target;
							root->branch_t = after;
							insert_breakpoint(context, root->branch_f, debugger);
						}
					}
				} else {
					after = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
				}
			}
			insert_breakpoint(context, after, debugger);
			return 0;
		case 's':
			if (input_buf[1] == 'e') {
				param = find_param(input_buf);
				if (!param) {
					fputs("Missing destination parameter for set\n", stderr);
					return 1;
				}
				char *val = find_param(param);
				if (!val) {
					fputs("Missing value parameter for set\n", stderr);
					return 1;
				}
				long int_val;
				int reg_num;
				switch (val[0])
				{
				case 'd':
				case 'a':
					reg_num = val[1] - '0';
					if (reg_num < 0 || reg_num > 8) {
						fprintf(stderr, "Invalid register %s\n", val);
						return 1;
					}
					int_val = (val[0] == 'd' ? context->dregs : context->aregs)[reg_num];
					break;
				case '$':
					int_val = strtol(val+1, NULL, 16);
					break;
				case '0':
					if (val[1] == 'x') {
						int_val = strtol(val+2, NULL, 16);
						break;
					}
				default:
					int_val = strtol(val, NULL, 10);
				}
				switch(param[0])
				{
				case 'd':
				case 'a':
					reg_num = param[1] - '0';
					if (reg_num < 0 || reg_num > 8) {
						fprintf(stderr, "Invalid register %s\n", param);
						return 1;
					}
					(param[0] == 'd' ? context->dregs : context->aregs)[reg_num] = int_val;
					break;
				default:
					fprintf(stderr, "Invalid destinatino %s\n", param);
				}
				break;
			} else if (input_buf[1] == 'r') {
				system->header.soft_reset(&system->header);
				return 0;
			} else {
				if (inst.op == M68K_RTS) {
					after = m68k_read_long(context->aregs[7], context);
				} else if (inst.op == M68K_RTE || inst.op == M68K_RTR) {
					after = m68k_read_long(context->aregs[7] + 2, context);
				} else if(m68k_is_branch(&inst)) {
					if (inst.op == M68K_BCC && inst.extra.cond != COND_TRUE) {
						root->branch_f = after;
						root->branch_t = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
						insert_breakpoint(context, root->branch_t, debugger);
					} else if(inst.op == M68K_DBCC) {
						if (inst.extra.cond == COND_FALSE) {
							if (context->dregs[inst.dst.params.regs.pri] & 0xFFFF) {
								after = m68k_branch_target(&inst, context->dregs, context->aregs);
							}
						} else {
							root->branch_t = after;
							root->branch_f = m68k_branch_target(&inst, context->dregs, context->aregs);
							insert_breakpoint(context, root->branch_f, debugger);
						}
					} else {
						after = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
					}
				}
				insert_breakpoint(context, after, debugger);
				return 0;
			}
		case '?':
			print_m68k_help();
			break;
		case 'q':
			puts("Quitting");
			exit(0);
			break;
		default: {
			if (context->system == current_system) {
				//primary 68K for current system
				return run_genesis_debugger_command(context, address, input_buf);
			} else {
				//presumably Sega CD sub CPU
				//TODO: consider making this more generic
				return run_subcpu_debugger_command(context, address, input_buf);
			}
			break;
		}
	}
	return 1;
}

void print_m68k_help()
{
	printf("M68k Debugger Commands\n");
	printf("    b ADDRESS            - Set a breakpoint at ADDRESS\n");
	printf("    d BREAKPOINT         - Delete a 68K breakpoint\n");
	printf("    co BREAKPOINT        - Run a list of debugger commands each time\n");
	printf("                           BREAKPOINT is hit\n");
	printf("    a ADDRESS            - Advance to address\n");
	printf("    n                    - Advance to next instruction\n");
	printf("    o                    - Advance to next instruction ignoring branches to\n");
	printf("                           lower addresses (good for breaking out of loops)\n");
	printf("    s                    - Advance to next instruction (follows bsr/jsr)\n");
	printf("    se REG|ADDRESS VALUE - Set value\n");
	printf("    sr                   - Soft reset\n");
	printf("    c                    - Continue\n");
	printf("    bt                   - Print a backtrace\n");
	printf("    p[/(x|X|d|c)] VALUE  - Print a register or memory location\n");
	printf("    di[/(x|X|d|c)] VALUE - Print a register or memory location each time\n");
	printf("                           a breakpoint is hit\n");
	printf("    vs                   - Print VDP sprite list\n");
	printf("    vr                   - Print VDP register info\n");
	printf("    yc [CHANNEL NUM]     - Print YM-2612 channel info\n");
	printf("    yt                   - Print YM-2612 timer info\n");
	printf("    zb ADDRESS           - Set a Z80 breakpoint\n");
	printf("    zp[/(x|X|d|c)] VALUE - Display a Z80 value\n");
	printf("    ?                    - Display help\n");
	printf("    q                    - Quit BlastEm\n");
}

void print_z80_help()
{
	printf("Z80 Debugger Commands\n");
	printf("    b  ADDRESS           - Set a breakpoint at ADDRESS\n");
	printf("    de BREAKPOINT        - Delete a Z80 breakpoint\n");
	printf("    a  ADDRESS           - Advance to address\n");
	printf("    n                    - Advance to next instruction\n");
	printf("    c                    - Continue\n");
	printf("    p[/(x|X|d|c)] VALUE  - Print a register or memory location\n");
	printf("    di[/(x|X|d|c)] VALUE - Print a register or memory location each time\n");
	printf("                           a breakpoint is hit\n");
	printf("    q                    - Quit BlastEm\n");
}

void debugger(m68k_context * context, uint32_t address)
{
	static char last_cmd[1024];
	char input_buf[1024];
	m68kinst inst;

	init_terminal();

	context->options->sync_components(context, 0);
	if (context->system == current_system) {
		genesis_context *gen = context->system;
		vdp_force_update_framebuffer(gen->vdp);
	}
	debug_root *root = find_root(context);
	if (!root) {
		return;
	}
	//probably not necessary, but let's play it safe
	address &= 0xFFFFFF;
	if (address == root->branch_t) {
		bp_def ** f_bp = find_breakpoint(&root->breakpoints, root->branch_f);
		if (!*f_bp) {
			remove_breakpoint(context, root->branch_f);
		}
		root->branch_t = root->branch_f = 0;
	} else if(address == root->branch_f) {
		bp_def ** t_bp = find_breakpoint(&root->breakpoints, root->branch_t);
		if (!*t_bp) {
			remove_breakpoint(context, root->branch_t);
		}
		root->branch_t = root->branch_f = 0;
	}

	uint32_t after = m68k_decode(m68k_instruction_fetch, context, &inst, address);
	int debugging = 1;
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address);
	if (*this_bp) {

		if ((*this_bp)->commands)
		{
			char *commands = strdup((*this_bp)->commands);
			char *copy = commands;

			while (debugging && *commands)
			{
				char *cmd = commands;
				strip_nl(cmd);
				commands += strlen(cmd) + 1;
				debugging = run_debugger_command(context, address, cmd, inst, after);
			}
			free(copy);
		}
		if (debugging) {
			printf("68K Breakpoint %d hit\n", (*this_bp)->index);
		} else {
			return;
		}
	} else {
		remove_breakpoint(context, address);
	}
	for (disp_def * cur = root->displays; cur; cur = cur->next) {
		debugger_print(context, cur->format_char, cur->param, address);
	}
	m68k_disasm(&inst, input_buf);
	printf("%X: %s\n", address, input_buf);
#ifdef _WIN32
#define prompt 1
#else
	int prompt = 1;
	fd_set read_fds;
	FD_ZERO(&read_fds);
	struct timeval timeout;
#endif
	while (debugging) {
		if (prompt) {
			fputs(">", stdout);
			fflush(stdout);
		}
		process_events();
#ifndef _WIN32
		timeout.tv_sec = 0;
		timeout.tv_usec = 16667;
		FD_SET(fileno(stdin), &read_fds);
		if(select(fileno(stdin) + 1, &read_fds, NULL, NULL, &timeout) < 1) {
			prompt = 0;
			continue;
		} else {
			prompt = 1;
		}
#endif
		if (!fgets(input_buf, sizeof(input_buf), stdin)) {
			fputs("fgets failed", stderr);
			break;
		}
		strip_nl(input_buf);
		//hitting enter repeats last command
		if (input_buf[0]) {
			strcpy(last_cmd, input_buf);
		} else {
			strcpy(input_buf, last_cmd);
		}
		debugging = run_debugger_command(context, address, input_buf, inst, after);
	}
	return;
}
