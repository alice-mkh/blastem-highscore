#include "debug.h"
#include "genesis.h"
#include "68kinst.h"
#include "segacd.h"
#include "blastem.h"
#include "bindings.h"
#include <ctype.h>
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

static const char *token_type_names[] = {
	"TOKEN_NONE",
	"TOKEN_NUM",
	"TOKEN_NAME",
	"TOKEN_OPER",
	"TOKEN_SIZE",
	"TOKEN_LBRACKET",
	"TOKEN_RBRACKET",
	"TOKEN_LPAREN",
	"TOKEN_RPAREN"
};

static token parse_token(char *start, char **end)
{
	while(*start && isblank(*start) && *start != '\n' && *start != '\r')
	{
		++start;
	}
	if (!*start || *start == '\n' || *start == '\r') {
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
	case '>':
	case '<':
		if ((*start == '!' || *start == '>' || *start == '<') && start[1] == '=') {
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
	case '(':
		*end = start + 1;
		return (token) {
			.type = TOKEN_LPAREN
		};
	case ')':
		*end = start + 1;
		return (token) {
			.type = TOKEN_RPAREN
		};
	}
	*end = start + 1;
	while (**end && !isspace(**end))
	{
		uint8_t done = 0;
		switch (**end)
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
		case '>':
		case '<':
		case '.':
			done = 1;
			break;
		}
		if (done) {
			break;
		}

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
	if (first.type == TOKEN_LPAREN) {
		expr *ret = parse_expression(after_first, end);
		if (!ret) {
			fprintf(stderr, "Expression expected after `(`\n");
			return NULL;
		}
		token rparen = parse_token(*end, end);
		if (rparen.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing closing `)`");
			free_expr(ret);
			return NULL;
		}
		return ret;
	}
	if (first.type != TOKEN_NUM && first.type != TOKEN_NAME) {
		fprintf(stderr, "Unexpected token %s\n", token_type_names[first.type]);
		return NULL;
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
	case '>':
	case '<':
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
		return maybe_muldiv(ret, *end, end);
	}
	if (first.type == TOKEN_LPAREN) {
		expr *ret = parse_expression(after_first, end);
		if (!ret) {
			fprintf(stderr, "Expression expected after `(`\n");
			return NULL;
		}
		token rparen = parse_token(*end, end);
		if (rparen.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing closing `)`");
			free_expr(ret);
			return NULL;
		}
		return maybe_muldiv(ret, *end, end);
	}
	if (first.type != TOKEN_NUM && first.type != TOKEN_NAME) {
		fprintf(stderr, "Unexpected token %s\n", token_type_names[first.type]);
		return NULL;
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
		case '>':
		case '<':
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
		return maybe_binary(ret, *end, end);
	}
	if (first.type == TOKEN_LPAREN) {
		expr *ret = parse_expression(after_first, end);
		if (!ret) {
			fprintf(stderr, "Expression expected after `(`\n");
			return NULL;
		}
		token rparen = parse_token(*end, end);
		if (rparen.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing closing `)`");
			free_expr(ret);
			return NULL;
		}
		return maybe_binary(ret, *end, end);
	}
	if (first.type != TOKEN_NUM && first.type != TOKEN_NAME) {
		fprintf(stderr, "Unexpected token %s\n", token_type_names[first.type]);
		return NULL;
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
			if (!bin->right) {
				fprintf(stderr, "Expected expression to the right of %s\n", second.v.op);
				free_expr(bin);
				return NULL;
			}
			return maybe_binary(bin, *end, end);
		case '+':
		case '-':
			bin->right = parse_scalar_or_muldiv(after_second, end);
			if (!bin->right) {
				fprintf(stderr, "Expected expression to the right of %s\n", second.v.op);
				free_expr(bin);
				return NULL;
			}
			return maybe_binary(bin, *end, end);
		case '=':
		case '!':
		case '>':
		case '<':
			bin->right = parse_expression(after_second, end);
			if (!bin->right) {
				fprintf(stderr, "Expected expression to the right of %s\n", second.v.op);
				free_expr(bin);
				return NULL;
			}
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
		if (second.type == TOKEN_NAME) {
			free(second.v.str);
		}
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		*end = after_first;
		return ret;
	}
}

uint8_t eval_expr(debug_root *root, expr *e, uint32_t *out)
{
	uint32_t right;
	switch(e->type)
	{
	case EXPR_SCALAR:
		if (e->op.type == TOKEN_NAME) {
			return root->resolve(root, e->op.v.str, out);
		} else {
			*out = e->op.v.num;
			return 1;
		}
	case EXPR_UNARY:
		if (!eval_expr(root, e->left, out)) {
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
		if (!eval_expr(root, e->left, out) || !eval_expr(root, e->right, &right)) {
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
		case '>':
			*out = e->op.v.op[1] ? *out >= right : *out > right;
			break;
		case '<':
			*out = e->op.v.op[1] ? *out <= right : *out < right;
			break;
		default:
			return 0;
		}
		return 1;
	case EXPR_SIZE:
		if (!eval_expr(root, e->left, out)) {
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
		if (!eval_expr(root, e->left, out)) {
			return 0;
		}
		return root->read_mem(root, out, e->op.v.op[0]);
	default:
		return 0;
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

static uint8_t read_m68k(debug_root *root, uint32_t *out, char size)
{
	m68k_context *context = root->cpu_context;
	if (size == 'b') {
		*out = m68k_read_byte(*out, context);
	} else if (size == 'l') {
		if (*out & 1) {
			fprintf(stderr, "Longword access to odd addresses (%X) is not allowed\n", *out);
			return 0;
		}
		*out = m68k_read_long(*out, context);
	} else {
		if (*out & 1) {
			fprintf(stderr, "Wword access to odd addresses (%X) is not allowed\n", *out);
			return 0;
		}
		*out = m68k_read_word(*out, context);
	}
	return 1;
}

static uint8_t write_m68k(debug_root *root, uint32_t address, uint32_t value, char size)
{
	m68k_context *context = root->cpu_context;
	if (size == 'b') {
		write_byte(address, value, (void **)context->mem_pointers, &context->options->gen, context);
	} else if (size == 'l') {
		if (address & 1) {
			fprintf(stderr, "Longword access to odd addresses (%X) is not allowed\n", address);
			return 0;
		}
		write_word(address, value >> 16, (void **)context->mem_pointers, &context->options->gen, context);
		write_word(address + 2, value, (void **)context->mem_pointers, &context->options->gen, context);
	} else {
		if (address & 1) {
			fprintf(stderr, "Wword access to odd addresses (%X) is not allowed\n", address);
			return 0;
		}
		write_word(address, value, (void **)context->mem_pointers, &context->options->gen, context);
	}
	return 1;
}

static uint8_t resolve_m68k(debug_root *root, const char *name, uint32_t *out)
{
	m68k_context *context = root->cpu_context;
	if ((name[0] == 'd' || name[0] == 'D') && name[1] >= '0' && name[1] <= '7' && !name[2]) {
		*out = context->dregs[name[1]-'0'];
	} else if ((name[0] == 'a' || name[0] == 'A') && name[1] >= '0' && name[1] <= '7' && !name[2]) {
		*out = context->aregs[name[1]-'0'];
	} else if (!strcasecmp(name, "sr")) {
		*out = context->status << 8;
		for (int flag = 0; flag < 5; flag++) {
			*out |= context->flags[flag] << (4-flag);
		}
	} else if(!strcasecmp(name, "cycle")) {
		*out = context->current_cycle;
	} else if (!strcasecmp(name, "pc")) {
		*out = root->address;
	} else if (!strcasecmp(name, "usp")) {
		*out = context->status & 0x20 ? context->aregs[8] : context->aregs[7];
	} else if (!strcasecmp(name, "ssp")) {
		*out = context->status & 0x20 ? context->aregs[7] : context->aregs[8];
	} else {
		return 0;
	}
	return 1;
}

static uint8_t set_m68k(debug_root *root, const char *name, uint32_t value)
{
	m68k_context *context = root->cpu_context;
	if ((name[0] == 'd' || name[0] == 'D') && name[1] >= '0' && name[1] <= '7' && !name[2]) {
		context->dregs[name[1]-'0'] = value;
	} else if ((name[0] == 'a' || name[0] == 'A') && name[1] >= '0' && name[1] <= '7' && !name[2]) {
		context->aregs[name[1]-'0'] = value;
	} else if (!strcasecmp(name, "sr")) {
		context->status = value >> 8;
		for (int flag = 0; flag < 5; flag++) {
			context->flags[flag] = (value & (1 << (4 - flag))) != 0;
		}
	} else if (!strcasecmp(name, "usp")) {
		context->aregs[context->status & 0x20 ? 8 : 7] = value;
	} else if (!strcasecmp(name, "ssp")) {
		context->aregs[context->status & 0x20 ? 7 : 8] = value;
	} else {
		return 0;
	}
	return 1;
}

static uint8_t resolve_genesis(debug_root *root, const char *name, uint32_t *out)
{
	if (resolve_m68k(root, name, out)) {
		return 1;
	}
	m68k_context *m68k = root->cpu_context;
	genesis_context *gen = m68k->system;
	if (!strcmp(name, "f") || !strcmp(name, "frame")) {
		*out = gen->vdp->frame;
		return 1;
	}
	return 0;
}

void ambiguous_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	char *prefix = data;
	char * full = alloc_concat(prefix, key);
	fprintf(stderr, "\t%s\n", full);
	free(full);
}

uint8_t parse_command(debug_root *root, char *text, parsed_command *out)
{
	char *cur = text;
	while (*cur && *cur != '/' && !isspace(*cur))
	{
		++cur;
	}
	char *name = malloc(cur - text + 1);
	memcpy(name, text, cur - text);
	name[cur-text] = 0;
	uint8_t ret = 0;
	tern_node *prefix_res = tern_find_prefix(root->commands, name);
	command_def *def = tern_find_ptr(prefix_res, "");
	if (!def) {
		tern_node *node = prefix_res;
		while (node)
		{
			if (node->left || node->right) {
				break;
			}
			if (node->el) {
				node = node->straight.next;
			} else {
				def = node->straight.value.ptrval;
				break;
			}
		}
		if (!def && prefix_res) {
			fprintf(stderr, "%s is ambiguous. Matching commands:\n", name);
			tern_foreach(prefix_res, ambiguous_iter, name);
			goto cleanup_name;
		}
	}
	if (!def) {
		fprintf(stderr, "%s is not a recognized command\n", name);
		goto cleanup_name;
	}
	char *format = NULL;
	if (*cur == '/') {
		++cur;
		text = cur;
		while (*cur && !isspace(*cur))
		{
			++cur;
		}
		format = malloc(cur - text + 1);
		memcpy(format, text, cur - text);
		format[cur - text] = 0;
	}
	int num_args = 0;
	command_arg *args = NULL;
	if (*cur && *cur != '\n') {
		++cur;
	}
	text = cur;
	if (def->raw_args) {
		while (*cur && *cur != '\n')
		{
			++cur;
		}
		char *raw_param = NULL;
		if (cur != text) {
			raw_param = malloc(cur - text + 1);
			memcpy(raw_param, text, cur - text);
			raw_param[cur - text] = 0;
		}
		out->raw = raw_param;
		out->args = NULL;
		out->num_args = 0;
	} else {
		int arg_storage = 0;
		if (def->max_args > 0) {
			arg_storage = def->max_args;
		} else if (def->max_args) {
			arg_storage = def->min_args > 0 ? 2 * def->min_args : 2;
		}
		if (arg_storage) {
			args = calloc(arg_storage, sizeof(command_arg));
		}
		while (*text && *text != '\n')
		{
			char *after;
			expr *e = parse_expression(text, &after);
			if (e) {
				if (num_args == arg_storage) {
					if (def->max_args >= 0) {
						free_expr(e);
						fprintf(stderr, "Command %s takes a max of %d arguments, but at least %d provided\n", name, def->max_args, def->max_args+1);
						goto cleanup_args;
					} else {
						arg_storage *= 2;
						args = realloc(args, arg_storage * sizeof(command_arg));
					}
				}
				args[num_args].parsed = e;
				args[num_args].raw = malloc(after - text + 1);
				memcpy(args[num_args].raw, text, after - text);
				args[num_args++].raw[after - text] = 0;
				text = after;
			} else {
				goto cleanup_args;
			}
		}
		if (num_args < def->min_args) {
			fprintf(stderr, "Command %s requires at least %d arguments, but only %d provided\n", name, def->min_args, num_args);
			goto cleanup_args;
		}
		out->raw = NULL;
		out->args = args;
		out->num_args = num_args;
	}
	out->def = def;
	out->format = format;

	ret = 1;
cleanup_args:
	if (!ret) {
		for (int i = 0; i < num_args; i++)
		{
			free_expr(args[i].parsed);
			free(args[i].raw);
		}
		free(args);
	}
cleanup_name:
	free(name);
	return ret;
}

static void free_parsed_command(parsed_command *cmd);
static void free_command_block(command_block *block)
{
	for (int i = 0; i < block->num_commands; i++)
	{
		free_parsed_command(block->commands + i);
	}
	free(block->commands);
}

static void free_parsed_command(parsed_command *cmd)
{
	free(cmd->format);
	free(cmd->raw);
	for (int i = 0; i < cmd->num_args; i++)
	{
		free(cmd->args[i].raw);
		free_expr(cmd->args[i].parsed);
	}
	free_command_block(&cmd->block);
	free_command_block(&cmd->else_block);
	free(cmd->args);
}

enum {
	READ_FAILED = 0,
	NORMAL,
	EMPTY,
	ELSE,
	END
};

static uint8_t read_parse_command(debug_root *root, parsed_command *out, int indent_level)
{
	++indent_level;
	for (int i = 0; i < indent_level; i++)
	{
		putchar('>');
	}
	putchar(' ');
	fflush(stdout);
#ifdef _WIN32
#define wait 0
#else
	int wait = 1;
	fd_set read_fds;
	FD_ZERO(&read_fds);
	struct timeval timeout;
#endif
	do {
		process_events();
#ifndef _WIN32
		timeout.tv_sec = 0;
		timeout.tv_usec = 16667;
		FD_SET(fileno(stdin), &read_fds);
		if(select(fileno(stdin) + 1, &read_fds, NULL, NULL, &timeout) >= 1) {
			wait = 0;
		}
#endif
	} while (wait);

	char input_buf[1024];
	if (!fgets(input_buf, sizeof(input_buf), stdin)) {
		fputs("fgets failed", stderr);
		return READ_FAILED;
	}
	char *stripped = strip_ws(input_buf);
	if (!stripped[0]) {
		return EMPTY;
	}
	if (indent_level > 1) {
		if (!strcmp(stripped, "else")) {
			return ELSE;
		}
		if (!strcmp(stripped, "end")) {
			return END;
		}
	}
	if (parse_command(root, input_buf, out)) {
		if (!out->def->has_block) {
			return NORMAL;
		}
		int command_storage = 4;
		command_block *block = &out->block;
		block->commands = calloc(command_storage, sizeof(parsed_command));
		block->num_commands = 0;
		for (;;)
		{
			if (block->num_commands == command_storage) {
				command_storage *= 2;
				block->commands = realloc(block->commands, command_storage * sizeof(parsed_command));
			}
			switch (read_parse_command(root, block->commands + block->num_commands, indent_level))
			{
			case READ_FAILED:
				return READ_FAILED;
			case NORMAL:
				block->num_commands++;
				break;
			case END:
				return NORMAL;
			case ELSE:
				if (block == &out->else_block) {
					fprintf(stderr, "Too many else blocks for command %s\n", out->def->names[0]);
					return READ_FAILED;
				}
				if (!out->def->accepts_else) {
					fprintf(stderr, "Command %s does not take an else block\n", out->def->names[0]);
					return READ_FAILED;
				}
				block = &out->else_block;
				block->commands = calloc(command_storage, sizeof(parsed_command));
				block->num_commands = 0;
				break;
			}
		}
	}
	return READ_FAILED;
}

static uint8_t run_command(debug_root *root, parsed_command *cmd)
{
	if (!cmd->def->raw_args && !cmd->def->skip_eval) {
		for (int i = 0; i < cmd->num_args; i++)
		{
			if (!eval_expr(root, cmd->args[i].parsed, &cmd->args[i].value)) {
				fprintf(stderr, "Failed to eval %s\n", cmd->args[i].raw);
				return 1;
			}
		}
	}
	return cmd->def->impl(root, cmd);
}

static void debugger_repl(debug_root *root)
{

	int debugging = 1;
	parsed_command cmds[2] = {0};
	int cur = 0;
	uint8_t has_last = 0;
	if (root->last_cmd.def) {
		memcpy(cmds + 1, &root->last_cmd, sizeof(root->last_cmd));
		has_last = 1;
	}
	while(debugging) {
		switch (read_parse_command(root, cmds + cur, 0))
		{
		case NORMAL:
			debugging = run_command(root, cmds + cur);
			cur = !cur;
			if (debugging && has_last) {
				free_parsed_command(cmds + cur);
				memset(cmds + cur, 0, sizeof(cmds[cur]));
			}
			has_last = 1;
			break;
		case EMPTY:
			if (has_last) {
				debugging = run_command(root, cmds + !cur);
			}
			break;
		}
	}
	if (has_last) {
		memcpy(&root->last_cmd, cmds + !cur, sizeof(root->last_cmd));
	} else {
		free_parsed_command(cmds + !cur);
	}
	free_parsed_command(cmds + cur);
}

static uint8_t cmd_quit(debug_root *root, parsed_command *cmd)
{
	exit(0);
}

typedef struct {
	size_t num_commands;
	size_t longest_command;
} help_state;

static void help_first_pass(char *key, tern_val val, uint8_t valtype, void *data)
{
	command_def *def = val.ptrval;
	if (def->visited) {
		return;
	}
	def->visited = 1;
	help_state *state = data;
	state->num_commands++;
	size_t len = strlen(def->usage);
	if (len > state->longest_command) {
		state->longest_command = len;
	}
}

static void help_reset_visited(char *key, tern_val val, uint8_t valtype, void *data)
{
	command_def *def = val.ptrval;
	def->visited = 0;
}

static void help_second_pass(char *key, tern_val val, uint8_t valtype, void *data)
{
	command_def *def = val.ptrval;
	if (def->visited) {
		return;
	}
	def->visited = 1;
	help_state *state = data;
	size_t len = strlen(def->usage);
	printf("  %s", def->usage);
	while (len < state->longest_command) {
		putchar(' ');
		len++;
	}
	int remaining = 80 - state->longest_command - 5;
	const char *extra_desc = NULL;
	if (strlen(def->desc) <= remaining) {
		printf(" - %s\n", def->desc);
	} else {
		char split[76];
		int split_point = remaining;
		while (split_point > 0 && !isspace(def->desc[split_point]))
		{
			--split_point;
		}
		if (!split_point) {
			split_point = remaining;
		}
		memcpy(split, def->desc, split_point);
		extra_desc = def->desc + split_point + 1;
		split[split_point] = 0;
		printf(" - %s\n", split);
	}
	if (def->names[1]) {
		fputs("    Aliases: ", stdout);
		len = strlen("    Aliases: ");
		const char **name = def->names + 1;
		uint8_t first = 1;
		while (*name)
		{
			if (first) {
				first = 0;
			} else {
				putchar(',');
				putchar(' ');
				len += 2;
			}
			fputs(*name, stdout);
			len += strlen(*name);
			++name;
		}
	} else {
		len = 0;
	}
	if (extra_desc) {
		while (len < state->longest_command + 5) {
			putchar(' ');
			len++;
		}
		fputs(extra_desc, stdout);
	}
	putchar('\n');
}

static uint8_t cmd_help(debug_root *root, parsed_command *cmd)
{
	help_state state = {0,0};
	tern_foreach(root->commands, help_first_pass, &state);
	tern_foreach(root->commands, help_reset_visited, &state);
	tern_foreach(root->commands, help_second_pass, &state);
	tern_foreach(root->commands, help_reset_visited, &state);
	return 1;
}

static uint8_t cmd_continue(debug_root *root, parsed_command *cmd)
{
	return 0;
}

static void make_format_str(char *format_str, char *format)
{
	strcpy(format_str, "%s: %d\n");
	if (format) {
		switch (format[0])
		{
		case 'x':
		case 'X':
		case 'd':
		case 'c':
		case 's':
			format_str[5] = format[0];
			break;
		default:
			fprintf(stderr, "Unrecognized format character: %c\n", format[0]);
		}
	}
}

static void do_print(debug_root *root, char *format_str, char *raw, uint32_t value)
{
	if (format_str[5] == 's') {
		char tmp[128];
		int j;
		uint32_t addr = value;
		for (j = 0; j < sizeof(tmp)-1; j++, addr++)
		{
			uint32_t tmp_addr = addr;
			root->read_mem(root, &tmp_addr, 'b');
			char c = tmp_addr;
			if (c < 0x20 || c > 0x7F) {
				break;
			}
			tmp[j] = c;
		}
		tmp[j] = 0;
		printf(format_str, raw, tmp);
	} else {
		printf(format_str, raw, value);
	}
}

static uint8_t cmd_print(debug_root *root, parsed_command *cmd)
{
	char format_str[8];
	make_format_str(format_str, cmd->format);
	for (int i = 0; i < cmd->num_args; i++)
	{
		do_print(root, format_str, cmd->args[i].raw, cmd->args[i].value);
	}
	return 1;
}

static uint8_t cmd_printf(debug_root *root, parsed_command *cmd)
{
	char *param = cmd->raw;
	if (!param) {
		fputs("printf requires at least one parameter\n", stderr);
		return 1;
	}
	while (isblank(*param))
	{
		++param;
	}
	if (*param != '"') {
		fprintf(stderr, "First parameter to printf must be a string, found '%s'\n", param);
		return 1;
	}
	++param;
	char *fmt = strdup(param);
	char *cur = param, *out = fmt;
	while (*cur && *cur != '"')
	{
		if (*cur == '\\') {
			switch (cur[1])
			{
			case 't':
				*(out++) = '\t';
				break;
			case 'n':
				*(out++) = '\n';
				break;
			case 'r':
				*(out++) = '\r';
				break;
			case '\\':
				*(out++) = '\\';
				break;
			default:
				fprintf(stderr, "Unsupported escape character %c in string %s\n", cur[1], fmt);
				free(fmt);
				return 1;
			}
			cur += 2;
		} else {
			*(out++) = *(cur++);
		}
	}
	*out = 0;
	++cur;
	param = cur;
	cur = fmt;
	char format_str[3] = {'%', 'd', 0};
	while (*cur)
	{
		if (*cur == '%') {
			switch(cur[1])
			{
			case 'x':
			case 'X':
			case 'c':
			case 'd':
			case 's':
				break;
			default:
				fprintf(stderr, "Unsupported format character %c\n", cur[1]);
				free(fmt);
				return 1;
			}
			format_str[1] = cur[1];
			expr *arg = parse_expression(param, &param);
			if (!arg) {
				free(fmt);
				return 1;
			}
			uint32_t val;
			if (!eval_expr(root, arg, &val)) {
				free(fmt);
				return 1;
			}
			if (cur[1] == 's') {
				char tmp[128];
				int j;
				for (j = 0; j < sizeof(tmp)-1; j++, val++)
				{
					uint32_t addr = val;
					root->read_mem(root, &addr, 'b');
					char c = addr;
					if (c < 0x20 || c > 0x7F) {
						break;
					}
					tmp[j] = c;
				}
				tmp[j] = 0;
				printf(format_str, tmp);
			} else {
				printf(format_str, val);
			}
			cur += 2;
		} else {
			putchar(*cur);
			++cur;
		}
	}
	return 1;
}

static uint8_t cmd_display(debug_root *root, parsed_command *cmd)
{
	cmd_print(root, cmd);
	disp_def *ndisp = calloc(1, sizeof(*ndisp));
	ndisp->next = root->displays;
	ndisp->index = root->disp_index++;
	ndisp->format = cmd->format ? strdup(cmd->format) : NULL;
	ndisp->num_args = cmd->num_args;
	ndisp->args = cmd->args;
	cmd->args = NULL;
	cmd->num_args = 0;
	root->displays = ndisp;
	printf("Added display %d\n", ndisp->index);
	return 1;
}

static uint8_t cmd_delete_display(debug_root *root, parsed_command *cmd)
{
	disp_def **cur = &root->displays;
	while (*cur)
	{
		if ((*cur)->index == cmd->args[0].value) {
			disp_def *del_disp = *cur;
			*cur = del_disp->next;
			free(del_disp->format);
			for (int i = 0; i < del_disp->num_args; i++)
			{
				free(del_disp->args[i].raw);
				free_expr(del_disp->args[i].parsed);
			}
			free(del_disp->args);
			free(del_disp);
			break;
		} else {
			cur = &(*cur)->next;
		}
	}
	return 1;
}

static uint8_t cmd_softreset(debug_root *root, parsed_command *cmd)
{
	if (current_system->soft_reset) {
		current_system->soft_reset(current_system);
		return 0;
	} else {
		fputs("Current system does not support soft reset", stderr);
		return 1;
	}
}

static uint8_t cmd_command(debug_root *root, parsed_command *cmd)
{
	bp_def **target = find_breakpoint_idx(&root->breakpoints, cmd->args[0].value);
	if (!target) {
		fprintf(stderr, "Breakpoint %d does not exist!\n", cmd->args[0].value);
		return 1;
	}
	for (uint32_t i = 0; i < (*target)->num_commands; i++)
	{
		free_parsed_command((*target)->commands + i);
	}
	free((*target)->commands);
	(*target)->commands = cmd->block.commands;
	(*target)->num_commands = cmd->block.num_commands;
	cmd->block.commands = NULL;
	cmd->block.num_commands = 0;
	return 1;
}

static uint8_t execute_block(debug_root *root, command_block * block)
{
	uint8_t debugging = 1;
	for (int i = 0; i < block->num_commands; i++)
	{
		debugging = run_command(root, block->commands + i) && debugging;
	}
	return debugging;
}

static uint8_t cmd_if(debug_root *root, parsed_command *cmd)
{
	return execute_block(root, cmd->args[0].value ? &cmd->block : &cmd->else_block);
}

static uint8_t cmd_while(debug_root *root, parsed_command *cmd)
{
	if (!cmd->args[0].value) {
		return execute_block(root, &cmd->else_block);
	}
	int debugging = 1;
	do {
		debugging = execute_block(root, &cmd->block) && debugging;
		if (!eval_expr(root, cmd->args[0].parsed, &cmd->args[0].value)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[0].raw);
			return 1;
		}
	} while (cmd->args[0].value);
	return debugging;
}

const char *expr_type_names[] = {
	"EXPR_NONE",
	"EXPR_SCALAR",
	"EXPR_UNARY",
	"EXPR_BINARY",
	"EXPR_SIZE",
	"EXPR_MEM"
};

static uint8_t cmd_set(debug_root *root, parsed_command *cmd)
{
	char *name = NULL;
	char size = 0;
	uint32_t address;
	switch (cmd->args[0].parsed->type)
	{
	case EXPR_SCALAR:
		if (cmd->args[0].parsed->op.type == TOKEN_NAME) {
			name = cmd->args[0].parsed->op.v.str;
		} else {
			fputs("First argument to set must be a name or memory expression, not a number", stderr);
			return 1;
		}
		break;
	case EXPR_SIZE:
		size = cmd->args[0].parsed->op.v.op[0];
		if (cmd->args[0].parsed->left->op.type == TOKEN_NAME) {
			name = cmd->args[0].parsed->left->op.v.str;
		} else {
			fputs("First argument to set must be a name or memory expression, not a number", stderr);
			return 1;
		}
		break;
	case EXPR_MEM:
		size = cmd->args[0].parsed->op.v.op[0];
		if (!eval_expr(root, cmd->args[0].parsed->left, &address)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[0].raw);
			return 1;
		}
		break;
	default:
		fprintf(stderr, "First argument to set must be a name or memory expression, got %s\n", expr_type_names[cmd->args[0].parsed->type]);
		return 1;
	}
	if (!eval_expr(root, cmd->args[1].parsed, &cmd->args[1].value)) {
		fprintf(stderr, "Failed to eval %s\n", cmd->args[1].raw);
		return 1;
	}
	uint32_t value = cmd->args[1].value;
	if (name && size && size != 'l') {
		uint32_t old;
		if (!root->resolve(root, name, &old)) {
			fprintf(stderr, "Failed to eval %s\n", name);
			return 1;
		}
		if (size == 'b') {
			old &= 0xFFFFFF00;
			value &= 0xFF;
			value |= old;
		} else {
			old &= 0xFFFF0000;
			value &= 0xFFFF;
			value |= old;
		}
	}
	if (name) {
		if (!root->set(root, name, value)) {
			fprintf(stderr, "Failed to set %s\n", name);
		}
	} else if (!root->write_mem(root, address, value, size)) {
		fprintf(stderr, "Failed to write to address %X\n", address);
	}
	return 1;
}

static uint8_t cmd_frames(debug_root *root, parsed_command *cmd)
{
	current_system->enter_debugger_frames = cmd->args[0].value;
	return 0;
}

static uint8_t cmd_bindup(debug_root *root, parsed_command *cmd)
{
	if (!bind_up(cmd->raw)) {
		fprintf(stderr, "%s is not a valid binding name\n", cmd->raw);
	}
	return 1;
}

static uint8_t cmd_binddown(debug_root *root, parsed_command *cmd)
{
	if (!bind_down(cmd->raw)) {
		fprintf(stderr, "%s is not a valid binding name\n", cmd->raw);
	}
	return 1;
}

static uint8_t cmd_condition(debug_root *root, parsed_command *cmd)
{
	if (!eval_expr(root, cmd->args[0].parsed, &cmd->args[0].value)) {
		fprintf(stderr, "Failed to evaluate breakpoint number: %s\n", cmd->args[0].raw);
		return 1;
	}
	bp_def **target = find_breakpoint_idx(&root->breakpoints, cmd->args[0].value);
	if (!*target) {
		fprintf(stderr, "Failed to find breakpoint %u\n", cmd->args[0].value);
		return 1;
	}
	free_expr((*target)->condition);
	if (cmd->num_args > 1 && cmd->args[1].parsed) {
		(*target)->condition = cmd->args[1].parsed;
		cmd->args[1].parsed = NULL;
	} else {
		(*target)->condition = NULL;
	}
	return 1;
}

static uint8_t cmd_delete_m68k(debug_root *root, parsed_command *cmd)
{
	bp_def **this_bp = find_breakpoint_idx(&root->breakpoints, cmd->args[0].value);
	if (!*this_bp) {
		fprintf(stderr, "Breakpoint %d does not exist\n", cmd->args[0].value);
		return 1;
	}
	bp_def *tmp = *this_bp;
	remove_breakpoint(root->cpu_context, tmp->address);
	*this_bp = (*this_bp)->next;
	if (tmp->commands) {
		for (uint32_t i = 0; i < tmp->num_commands; i++)
		{
			free_parsed_command(tmp->commands + i);
		}
		free(tmp->commands);
	}
	free(tmp);
	return 1;
}

static uint8_t cmd_breakpoint_m68k(debug_root *root, parsed_command *cmd)
{
	insert_breakpoint(root->cpu_context, cmd->args[0].value, debugger);
	bp_def *new_bp = calloc(1, sizeof(bp_def));
	new_bp->next = root->breakpoints;
	new_bp->address = cmd->args[0].value;
	new_bp->index = root->bp_index++;
	root->breakpoints = new_bp;
	printf("68K Breakpoint %d set at %X\n", new_bp->index, cmd->args[0].value);
	return 1;
}

static uint8_t cmd_advance_m68k(debug_root *root, parsed_command *cmd)
{
	insert_breakpoint(root->cpu_context, cmd->args[0].value, debugger);
	return 0;
}

static uint8_t cmd_step_m68k(debug_root *root, parsed_command *cmd)
{
	m68kinst *inst = root->inst;
	m68k_context *context = root->cpu_context;
	uint32_t after = root->after;
	if (inst->op == M68K_RTS) {
		after = m68k_read_long(context->aregs[7], context);
	} else if (inst->op == M68K_RTE || inst->op == M68K_RTR) {
		after = m68k_read_long(context->aregs[7] + 2, context);
	} else if(m68k_is_branch(inst)) {
		if (inst->op == M68K_BCC && inst->extra.cond != COND_TRUE) {
			root->branch_f = after;
			root->branch_t = m68k_branch_target(inst, context->dregs, context->aregs) & 0xFFFFFF;
			insert_breakpoint(context, root->branch_t, debugger);
		} else if(inst->op == M68K_DBCC) {
			if (inst->extra.cond == COND_FALSE) {
				if (context->dregs[inst->dst.params.regs.pri] & 0xFFFF) {
					after = m68k_branch_target(inst, context->dregs, context->aregs);
				}
			} else {
				root->branch_t = after;
				root->branch_f = m68k_branch_target(inst, context->dregs, context->aregs);
				insert_breakpoint(context, root->branch_f, debugger);
			}
		} else {
			after = m68k_branch_target(inst, context->dregs, context->aregs) & 0xFFFFFF;
		}
	}
	insert_breakpoint(root->cpu_context, after, debugger);
	return 0;
}

static uint8_t cmd_over_m68k(debug_root *root, parsed_command *cmd)
{
	m68kinst *inst = root->inst;
	m68k_context *context = root->cpu_context;
	uint32_t after = root->after;
	if (inst->op == M68K_RTS) {
		after = m68k_read_long(context->aregs[7], context);
	} else if (inst->op == M68K_RTE || inst->op == M68K_RTR) {
		after = m68k_read_long(context->aregs[7] + 2, context);
	} else if(m68k_is_noncall_branch(inst)) {
		if (inst->op == M68K_BCC && inst->extra.cond != COND_TRUE) {
			root->branch_t = m68k_branch_target(inst, context->dregs, context->aregs)  & 0xFFFFFF;
			if (root->branch_t < after) {
					root->branch_t = 0;
			} else {
				root->branch_f = after;
				insert_breakpoint(context, root->branch_t, debugger);
			}
		} else if(inst->op == M68K_DBCC) {
			uint32_t target = m68k_branch_target(inst, context->dregs, context->aregs)  & 0xFFFFFF;
			if (target > after) {
				if (inst->extra.cond == COND_FALSE) {
					after = target;
				} else {
					root->branch_f = target;
					root->branch_t = after;
					insert_breakpoint(context, root->branch_f, debugger);
				}
			}
		} else {
			after = m68k_branch_target(inst, context->dregs, context->aregs) & 0xFFFFFF;
		}
	}
	insert_breakpoint(root->cpu_context, after, debugger);
	return 0;
}

static uint8_t cmd_next_m68k(debug_root *root, parsed_command *cmd)
{
	m68kinst *inst = root->inst;
	m68k_context *context = root->cpu_context;
	uint32_t after = root->after;
	if (inst->op == M68K_RTS) {
		after = m68k_read_long(context->aregs[7], context);
	} else if (inst->op == M68K_RTE || inst->op == M68K_RTR) {
		after = m68k_read_long(context->aregs[7] + 2, context);
	} else if(m68k_is_noncall_branch(inst)) {
		if (inst->op == M68K_BCC && inst->extra.cond != COND_TRUE) {
			root->branch_f = after;
			root->branch_t = m68k_branch_target(inst, context->dregs, context->aregs);
			insert_breakpoint(context, root->branch_t, debugger);
		} else if(inst->op == M68K_DBCC) {
			if ( inst->extra.cond == COND_FALSE) {
				if (context->dregs[inst->dst.params.regs.pri] & 0xFFFF) {
					after = m68k_branch_target(inst, context->dregs, context->aregs);
				}
			} else {
				root->branch_t = after;
				root->branch_f = m68k_branch_target(inst, context->dregs, context->aregs);
				insert_breakpoint(context, root->branch_f, debugger);
			}
		} else {
			after = m68k_branch_target(inst, context->dregs, context->aregs) & 0xFFFFFF;
		}
	}
	insert_breakpoint(root->cpu_context, after, debugger);
	return 0;
}

static uint8_t cmd_backtrace_m68k(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	uint32_t stack = context->aregs[7];
	uint8_t non_adr_count = 0;
	do {
		uint32_t bt_address = m68k_instruction_fetch(stack, context);
		bt_address = get_instruction_start(context->options, bt_address - 2);
		if (bt_address) {
			stack += 4;
			non_adr_count = 0;
			m68kinst inst;
			char buf[128];
			m68k_decode(m68k_instruction_fetch, context, &inst, bt_address);
			m68k_disasm(&inst, buf);
			printf("%X: %s\n", bt_address, buf);
		} else {
			//non-return address value on stack can be word wide
			stack += 2;
			non_adr_count++;
		}
		//TODO: Make sure we don't wander into an invalid memory region
	} while (stack && non_adr_count < 6);
	return 1;
}

static uint8_t cmd_vdp_sprites(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	genesis_context * gen = context->system;
	vdp_print_sprite_table(gen->vdp);
	return 1;
}

static uint8_t cmd_vdp_regs(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	genesis_context * gen = context->system;
	vdp_print_reg_explain(gen->vdp);
	return 1;
}

static uint8_t cmd_ym_channel(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	genesis_context * gen = context->system;
	if (cmd->num_args) {
		ym_print_channel_info(gen->ym, cmd->args[0].value - 1);
	} else {
		for (int i = 0; i < 6; i++) {
			ym_print_channel_info(gen->ym, i);
		}
	}
	return 1;
}

static uint8_t cmd_ym_timer(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	genesis_context * gen = context->system;
	ym_print_timer_info(gen->ym);
	return 1;
}

static uint8_t cmd_sub(debug_root *root, parsed_command *cmd)
{
	char *param = cmd->raw;
	while (param && *param && isblank(*param))
	{
		++param;
	}
	m68k_context *m68k = root->cpu_context;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (param && *param && !isspace(*param)) {
		parsed_command cmd = {0};
		debug_root *sub_root = find_m68k_root(cd->m68k);
		if (!sub_root) {
			fputs("Failed to get debug root for Sub CPU\n", stderr);
			return 1;
		}
		if (!parse_command(sub_root, param, &cmd)) {
			return 1;
		}
		uint8_t ret = run_command(sub_root, &cmd);
		free_parsed_command(&cmd);
		return ret;
	} else {
		cd->enter_debugger = 1;
		return 0;
	}
}

static uint8_t cmd_main(debug_root *root, parsed_command *cmd)
{
	char *param = cmd->raw;
	while (param && *param && isblank(*param))
	{
		++param;
	}
	m68k_context *m68k = root->cpu_context;
	segacd_context *cd = m68k->system;

	if (param && *param && !isspace(*param)) {
		parsed_command cmd = {0};
		debug_root *main_root = find_m68k_root(cd->genesis->m68k);
		if (!main_root) {
			fputs("Failed to get debug root for Main CPU\n", stderr);
			return 1;
		}
		if (!parse_command(main_root, param, &cmd)) {
			return 1;
		}
		uint8_t ret = run_command(main_root, &cmd);
		free_parsed_command(&cmd);
		return ret;
	} else {
		cd->genesis->header.enter_debugger = 1;
		return 0;
	}
}

static uint8_t cmd_gen_z80(debug_root *root, parsed_command *cmd)
{
	char *param = cmd->raw;
	while (param && *param && isblank(*param))
	{
		++param;
	}
	m68k_context *m68k = root->cpu_context;
	genesis_context *gen = m68k->system;

	if (param && *param && !isspace(*param)) {
		parsed_command cmd = {0};
		debug_root *z80_root = find_z80_root(gen->z80);
		if (!z80_root) {
			fputs("Failed to get debug root for Z80\n", stderr);
			return 1;
		}
		if (!parse_command(z80_root, param, &cmd)) {
			return 1;
		}
		uint8_t ret = run_command(z80_root, &cmd);
		free_parsed_command(&cmd);
		return ret;
	} else {
		gen->enter_z80_debugger = 1;
		return 0;
	}
}

command_def common_commands[] = {
	{
		.names = (const char *[]){
			"quit", NULL
		},
		.usage = "quit",
		.desc = "Quit BlastEm",
		.impl = cmd_quit,
		.min_args = 0,
		.max_args = 0,
	},
	{
		.names = (const char *[]){
			"help", "?", NULL
		},
		.usage = "help",
		.desc = "Print a list of available commands for the current debug context",
		.impl = cmd_help,
		.min_args = 0,
		.max_args = 1,
		.raw_args = 1
	},
	{
		.names = (const char *[]){
			"continue", "c", NULL
		},
		.usage = "continue",
		.desc = "Resume execution",
		.impl = cmd_continue,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"print", "p", NULL
		},
		.usage = "print[/FORMAT] EXPRESSION...",
		.desc = "Print one or more expressions using the optional format character",
		.impl = cmd_print,
		.min_args = 1,
		.max_args = -1
	},
	{
		.names = (const char *[]){
			"printf", NULL
		},
		.usage = "printf FORMAT EXPRESSION...",
		.desc = "Print a string with C-style formatting specifiers replaced with the value of the remaining arguments",
		.impl = cmd_printf,
		.min_args = 1,
		.max_args = -1,
		.raw_args = 1
	},
	{
		.names = (const char *[]){
			"softreset", "sr", NULL
		},
		.usage = "softreset",
		.desc = "Perform a soft-reset for the current system",
		.impl = cmd_softreset,
		.min_args = 0,
		.max_args = 0,
	},
	{
		.names = (const char *[]){
			"display", NULL
		},
		.usage = "display[/FORMAT] EXPRESSION...",
		.desc = "Print one or more expressions every time the debugger is entered",
		.impl = cmd_display,
		.min_args = 1,
		.max_args = -1
	},
	{
		.names = (const char *[]){
			"deletedisplay", "dd", NULL
		},
		.usage = "deletedisplay DISPLAYNUM",
		.desc = "Remove expressions added with the `display` command",
		.impl = cmd_delete_display,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"commands", NULL
		},
		.usage = "command BREAKPOINT",
		.desc = "Set a list of debugger commands to be executed when the given breakpoint is hit",
		.impl = cmd_command,
		.min_args = 1,
		.max_args = 1,
		.has_block = 1
	},
	{
		.names = (const char *[]){
			"set", NULL
		},
		.usage = "set MEM|NAME VALUE",
		.desc = "Set a register, symbol or memory location to the result of evaluating VALUE",
		.impl = cmd_set,
		.min_args = 2,
		.max_args = 2,
		.skip_eval = 1
	},
	{
		.names = (const char *[]){
			"frames", NULL
		},
		.usage = "frames EXPRESSION",
		.desc = "Resume execution for EXPRESSION video frames",
		.impl = cmd_frames,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"bindup", NULL
		},
		.usage = "bindup NAME",
		.desc = "Simulate a keyup for binding NAME",
		.impl = cmd_bindup,
		.min_args = 1,
		.max_args = 1,
		.raw_args = 1
	},
	{
		.names = (const char *[]){
			"binddown", NULL
		},
		.usage = "bindown NAME",
		.desc = "Simulate a keydown for binding NAME",
		.impl = cmd_binddown,
		.min_args = 1,
		.max_args = 1,
		.raw_args = 1
	},
	{
		.names = (const char *[]){
			"condition", NULL
		},
		.usage = "condition BREAKPOINT [EXPRESSION]",
		.desc = "Makes breakpoint BREAKPOINT conditional on the value of EXPRESSION or removes a condition if EXPRESSION is omitted",
		.impl = cmd_condition,
		.min_args = 1,
		.max_args = 2,
		.skip_eval = 1
	},
	{
		.names = (const char *[]){
			"if", NULL
		},
		.usage = "if CONDITION",
		.desc = "If the condition is true, the following block is executed. Otherwise the else block is executed if present",
		.impl = cmd_if,
		.min_args = 1,
		.max_args = 1,
		.has_block = 1,
		.accepts_else = 1
	},
	{
		.names = (const char *[]){
			"while", NULL
		},
		.usage = "while CONDITION",
		.desc = "The following block is executed repeatedly until the condition is false. If the condition is false at the start, the else block is executed if present",
		.impl = cmd_while,
		.min_args = 1,
		.max_args = 1,
		.has_block = 1,
		.accepts_else = 1
	}
};
#define NUM_COMMON (sizeof(common_commands)/sizeof(*common_commands))

command_def m68k_commands[] = {
	{
		.names = (const char *[]){
			"breakpoint", "b", NULL
		},
		.usage = "breakpoint ADDRESSS",
		.desc = "Set a breakpoint at ADDRESS",
		.impl = cmd_breakpoint_m68k,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"advance", NULL
		},
		.usage = "advance ADDRESS",
		.desc = "Advance to ADDRESS",
		.impl = cmd_advance_m68k,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"step", "s", NULL
		},
		.usage = "step",
		.desc = "Advance to the next instruction, stepping into subroutines",
		.impl = cmd_step_m68k,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"over", NULL
		},
		.usage = "over",
		.desc = "Advance to the next instruction, ignoring branches to lower addresses",
		.impl = cmd_over_m68k,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"next", NULL
		},
		.usage = "next",
		.desc = "Advance to the next instruction",
		.impl = cmd_next_m68k,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"backtrace", "bt", NULL
		},
		.usage = "backtrace",
		.desc = "Print a backtrace",
		.impl = cmd_backtrace_m68k,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"delete", "d", NULL
		},
		.usage = "delete BREAKPOINT",
		.desc = "Remove breakpoint identified by BREAKPOINT",
		.impl = cmd_delete_m68k,
		.min_args = 1,
		.max_args = 1
	}
};

#define NUM_68K (sizeof(m68k_commands)/sizeof(*m68k_commands))

command_def genesis_commands[] = {
	{
		.names = (const char *[]){
			"vdpsprites", "vs", NULL
		},
		.usage = "vdpsprites",
		.desc = "Print the VDP sprite table",
		.impl = cmd_vdp_sprites,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"vdpsregs", "vr", NULL
		},
		.usage = "vdpregs",
		.desc = "Print VDP register values with a short description",
		.impl = cmd_vdp_regs,
		.min_args = 0,
		.max_args = 0
	},
#ifndef NO_Z80
	{
		.names = (const char *[]){
			"z80", NULL
		},
		.usage = "z80 [COMMAND]",
		.desc = "Run a Z80 debugger command or switch to Z80 context when no command is given",
		.impl = cmd_gen_z80,
		.min_args = 0,
		.max_args = -1,
		.raw_args = 1
	},
#endif
	{
		.names = (const char *[]){
			"ymchannel", "yc", NULL
		},
		.usage = "ymchannel [CHANNEL]",
		.desc = "Print YM-2612 channel and operator params. Limited to CHANNEL if specified",
		.impl = cmd_ym_channel,
		.min_args = 0,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"ymtimer", "yt", NULL
		},
		.usage = "ymtimer",
		.desc = "Print YM-2612 timer info",
		.impl = cmd_ym_timer,
		.min_args = 0,
		.max_args = 0
	}
};

#define NUM_GENESIS (sizeof(genesis_commands)/sizeof(*genesis_commands))

command_def scd_main_commands[] = {
	{
		.names = (const char *[]){
			"subcpu", NULL
		},
		.usage = "subcpu [COMMAND]",
		.desc = "Run a Sub-CPU debugger command or switch to Sub-CPU context when no command is given",
		.impl = cmd_sub,
		.min_args = 0,
		.max_args = -1,
		.raw_args = 1
	}
};

#define NUM_SCD_MAIN (sizeof(scd_main_commands)/sizeof(*scd_main_commands))

command_def scd_sub_commands[] = {
	{
		.names = (const char *[]){
			"maincpu", NULL
		},
		.usage = "maincpu [COMMAND]",
		.desc = "Run a Main-CPU debugger command or switch to Main-CPU context when no command is given",
		.impl = cmd_main,
		.min_args = 0,
		.max_args = -1,
		.raw_args = 1
	}
};

#define NUM_SCD_SUB (sizeof(scd_main_commands)/sizeof(*scd_main_commands))

#ifndef NO_Z80

static uint8_t cmd_delete_z80(debug_root *root, parsed_command *cmd)
{
	bp_def **this_bp = find_breakpoint_idx(&root->breakpoints, cmd->args[0].value);
	if (!*this_bp) {
		fprintf(stderr, "Breakpoint %d does not exist\n", cmd->args[0].value);
		return 1;
	}
	bp_def *tmp = *this_bp;
	zremove_breakpoint(root->cpu_context, tmp->address);
	*this_bp = (*this_bp)->next;
	if (tmp->commands) {
		for (uint32_t i = 0; i < tmp->num_commands; i++)
		{
			free_parsed_command(tmp->commands + i);
		}
		free(tmp->commands);
	}
	free(tmp);
	return 1;
}

static uint8_t cmd_breakpoint_z80(debug_root *root, parsed_command *cmd)
{
	zinsert_breakpoint(root->cpu_context, cmd->args[0].value, (uint8_t *)zdebugger);
	bp_def *new_bp = calloc(1, sizeof(bp_def));
	new_bp->next = root->breakpoints;
	new_bp->address = cmd->args[0].value;
	new_bp->index = root->bp_index++;
	root->breakpoints = new_bp;
	printf("Z80 Breakpoint %d set at %X\n", new_bp->index, cmd->args[0].value);
	return 1;
}

static uint8_t cmd_advance_z80(debug_root *root, parsed_command *cmd)
{
	zinsert_breakpoint(root->cpu_context, cmd->args[0].value, (uint8_t *)zdebugger);
	return 0;
}

static uint8_t cmd_step_z80(debug_root *root, parsed_command *cmd)
{
	z80inst *inst = root->inst;
	z80_context *context = root->cpu_context;
	uint32_t after = root->after;
	//TODO: handle conditional branches
	if (inst->op == Z80_JP || inst->op == Z80_CALL || inst->op == Z80_RST) {
		if (inst->addr_mode == Z80_IMMED) {
			after = inst->immed;
		} else if (inst->ea_reg == Z80_HL) {
#ifndef NEW_CORE
			after = context->regs[Z80_H] << 8 | context->regs[Z80_L];
		} else if (inst->ea_reg == Z80_IX) {
			after = context->regs[Z80_IXH] << 8 | context->regs[Z80_IXL];
		} else if (inst->ea_reg == Z80_IY) {
			after = context->regs[Z80_IYH] << 8 | context->regs[Z80_IYL];
#endif
		}
	} else if(inst->op == Z80_JR) {
		after += inst->immed;
	} else if(inst->op == Z80_RET) {
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
	return 0;
}

static uint8_t cmd_over_z80(debug_root *root, parsed_command *cmd)
{
	fputs("not implemented yet\n", stderr);
	return 1;
}

static uint8_t cmd_next_z80(debug_root *root, parsed_command *cmd)
{
	z80inst *inst = root->inst;
	z80_context *context = root->cpu_context;
	uint32_t after = root->after;
	//TODO: handle conditional branches
	if (inst->op == Z80_JP) {
		if (inst->addr_mode == Z80_IMMED) {
			after = inst->immed;
		} else if (inst->ea_reg == Z80_HL) {
#ifndef NEW_CORE
			after = context->regs[Z80_H] << 8 | context->regs[Z80_L];
		} else if (inst->ea_reg == Z80_IX) {
			after = context->regs[Z80_IXH] << 8 | context->regs[Z80_IXL];
		} else if (inst->ea_reg == Z80_IY) {
			after = context->regs[Z80_IYH] << 8 | context->regs[Z80_IYL];
#endif
		}
	} else if(inst->op == Z80_JR) {
		after += inst->immed;
	} else if(inst->op == Z80_RET) {
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
	return 0;
}

static uint8_t cmd_backtrace_z80(debug_root *root, parsed_command *cmd)
{
	z80_context *context = root->cpu_context;
	uint32_t stack = context->sp;
	uint8_t non_adr_count = 0;
	do {
		uint32_t bt_address = stack;
		if (!root->read_mem(root, &bt_address, 'w')) {
			break;
		}
		bt_address = z80_get_instruction_start(context, bt_address - 1);
		if (bt_address != 0xFEEDFEED) {
			stack += 4;
			non_adr_count = 0;
			z80inst inst;
			char buf[128];
			uint8_t *pc = get_native_pointer(bt_address, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
			z80_decode(pc, &inst);
			z80_disasm(&inst, buf, bt_address);
			printf("%X: %s\n", bt_address, buf);
		} else {
			//non-return address value on stack can be byte wide
			stack++;
			non_adr_count++;
		}
		//TODO: Make sure we don't wander into an invalid memory region
	} while (stack && non_adr_count < 6);
	return 1;
}

static uint8_t cmd_gen_m68k(debug_root *root, parsed_command *cmd)
{
	char *param = cmd->raw;
	while (param && *param && isblank(*param))
	{
		++param;
	}
	genesis_context *gen = (genesis_context *)current_system;

	if (param && *param && !isspace(*param)) {
		parsed_command cmd = {0};
		debug_root *m68k_root = find_m68k_root(gen->m68k);
		if (!m68k_root) {
			fputs("Failed to get debug root for M68K\n", stderr);
			return 1;
		}
		if (!parse_command(m68k_root, param, &cmd)) {
			return 1;
		}
		uint8_t ret = run_command(m68k_root, &cmd);
		free_parsed_command(&cmd);
		return ret;
	} else {
		gen->header.enter_debugger = 1;
		return 0;
	}
}

command_def z80_commands[] = {
	{
		.names = (const char *[]){
			"breakpoint", "b", NULL
		},
		.usage = "breakpoint ADDRESSS",
		.desc = "Set a breakpoint at ADDRESS",
		.impl = cmd_breakpoint_z80,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"advance", NULL
		},
		.usage = "advance ADDRESS",
		.desc = "Advance to ADDRESS",
		.impl = cmd_advance_z80,
		.min_args = 1,
		.max_args = 1
	},
	{
		.names = (const char *[]){
			"step", "s", NULL
		},
		.usage = "step",
		.desc = "Advance to the next instruction, stepping into subroutines",
		.impl = cmd_step_z80,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"over", NULL
		},
		.usage = "over",
		.desc = "Advance to the next instruction, ignoring branches to lower addresses",
		.impl = cmd_over_z80,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"next", NULL
		},
		.usage = "next",
		.desc = "Advance to the next instruction",
		.impl = cmd_next_z80,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"backtrace", "bt", NULL
		},
		.usage = "backtrace",
		.desc = "Print a backtrace",
		.impl = cmd_backtrace_z80,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"delete", "d", NULL
		},
		.usage = "delete BREAKPOINT",
		.desc = "Remove breakpoint identified by BREAKPOINT",
		.impl = cmd_delete_z80,
		.min_args = 1,
		.max_args = 1
	}
};

#define NUM_Z80 (sizeof(z80_commands)/sizeof(*z80_commands))

command_def gen_z80_commands[] = {
	{
		.names = (const char *[]){
			"m68k", NULL
		},
		.usage = "m68k [COMMAND]",
		.desc = "Run a M68K debugger command or switch to M68K context when no command is given",
		.impl = cmd_gen_m68k,
		.min_args = 0,
		.max_args = -1,
		.raw_args = 1
	}
};

#define NUM_GEN_Z80 (sizeof(gen_z80_commands)/sizeof(*gen_z80_commands))

#endif

void add_commands(debug_root *root, command_def *defs, uint32_t num_commands)
{
	for (uint32_t i = 0; i < num_commands; i++)
	{
		for (int j = 0; defs[i].names[j]; j++)
		{
			root->commands = tern_insert_ptr(root->commands, defs[i].names[j], defs + i);
		}
	}
}

debug_root *find_m68k_root(m68k_context *context)
{
	debug_root *root = find_root(context);
	if (root && !root->commands) {
		add_commands(root, common_commands, NUM_COMMON);
		add_commands(root, m68k_commands, NUM_68K);
		root->read_mem = read_m68k;
		root->write_mem = write_m68k;
		root->set = set_m68k;
		switch (current_system->type)
		{
		case SYSTEM_GENESIS:
		case SYSTEM_SEGACD:
			//check if this is the main CPU
			if (context->system == current_system) {
				root->resolve = resolve_genesis;
				add_commands(root, genesis_commands, NUM_GENESIS);
				if (current_system->type == SYSTEM_SEGACD) {
					add_commands(root, scd_main_commands, NUM_SCD_MAIN);
				}
				break;
			} else {
				add_commands(root, scd_sub_commands, NUM_SCD_SUB);
			}
		default:
			root->resolve = resolve_m68k;
		}
	}
	return root;
}

#ifndef NO_Z80

static uint8_t read_z80(debug_root *root, uint32_t *out, char size)
{
	z80_context *context = root->cpu_context;
	uint32_t address = *out;
	*out = read_byte(address, (void **)context->mem_pointers, &context->options->gen, context);
	if (size == 'w') {
		*out |= read_byte(address + 1, (void **)context->mem_pointers, &context->options->gen, context) << 8;
	}
	return 1;
}

static uint8_t write_z80(debug_root *root, uint32_t address, uint32_t value, char size)
{
	z80_context *context = root->cpu_context;
	write_byte(address, value, (void **)context->mem_pointers, &context->options->gen, context);
	if (size == 'w') {
		write_byte(address + 1, value >> 8, (void **)context->mem_pointers, &context->options->gen, context);
	}
	return 1;
}

static uint8_t resolve_z80(debug_root *root, const char *name, uint32_t *out)
{
	z80_context *context = root->cpu_context;
	switch (tolower(name[0]))
	{
	case 'a':
		if (!name[1]) {
			*out = context->regs[Z80_A];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_A];
		} else if (tolower(name[1]) == 'f') {
			if (!name[2]) {
				*out = context->regs[Z80_A] << 8;
				*out |= context->flags[ZF_S] << 7;
				*out |= context->flags[ZF_Z] << 6;
				*out |= context->flags[ZF_H] << 4;
				*out |= context->flags[ZF_PV] << 2;
				*out |= context->flags[ZF_N] << 1;
				*out |= context->flags[ZF_C];
			} else if (name[2] == '\'' && !name[3]) {
				*out = context->alt_regs[Z80_A] << 8;
				*out |= context->alt_flags[ZF_S] << 7;
				*out |= context->alt_flags[ZF_Z] << 6;
				*out |= context->alt_flags[ZF_H] << 4;
				*out |= context->alt_flags[ZF_PV] << 2;
				*out |= context->alt_flags[ZF_N] << 1;
				*out |= context->alt_flags[ZF_C];
			} else {
				return 0;
			}
		} else {
			return 0;
		}
		break;
	case 'b':
		if (!name[1]) {
			*out = context->regs[Z80_B];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_B];
		} else if (tolower(name[1]) == 'c') {
			if (!name[2]) {
				*out = context->regs[Z80_B] << 8 | context->regs[Z80_C];
			} else if (name[2] == '\'' && !name[3]) {
				*out = context->alt_regs[Z80_B] << 8 | context->alt_regs[Z80_C];
			} else {
				return 0;
			}
		}
		break;
	case 'c':
		if (!name[1]) {
			*out = context->regs[Z80_C];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_C];
		} else {
			return 0;
		}
		break;
	case 'd':
		if (!name[1]) {
			*out = context->regs[Z80_D];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_D];
		} else if (tolower(name[1]) == 'e') {
			if (!name[2]) {
				*out = context->regs[Z80_D] << 8 | context->regs[Z80_E];
			} else if (name[2] == '\'' && !name[3]) {
				*out = context->alt_regs[Z80_D] << 8 | context->alt_regs[Z80_E];
			} else {
				return 0;
			}
		}
		break;
	case 'e':
		if (!name[1]) {
			*out = context->regs[Z80_E];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_E];
		} else {
			return 0;
		}
		break;
	case 'f':
		if (!name[1]) {
			*out = context->flags[ZF_S] << 7;
			*out |= context->flags[ZF_Z] << 6;
			*out |= context->flags[ZF_H] << 4;
			*out |= context->flags[ZF_PV] << 2;
			*out |= context->flags[ZF_N] << 1;
			*out |= context->flags[ZF_C];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_flags[ZF_S] << 7;
			*out |= context->alt_flags[ZF_Z] << 6;
			*out |= context->alt_flags[ZF_H] << 4;
			*out |= context->alt_flags[ZF_PV] << 2;
			*out |= context->alt_flags[ZF_N] << 1;
			*out |= context->alt_flags[ZF_C];
		} else {
			return 0;
		}
		break;
	case 'h':
		if (!name[1]) {
			*out = context->regs[Z80_H];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_H];
		} else if (tolower(name[1]) == 'e') {
			if (!name[2]) {
				*out = context->regs[Z80_H] << 8 | context->regs[Z80_L];
			} else if (name[2] == '\'' && !name[3]) {
				*out = context->alt_regs[Z80_H] << 8 | context->alt_regs[Z80_L];
			} else {
				return 0;
			}
		}
		break;
	case 'i':
		switch (tolower(name[1]))
		{
		case 0:
			*out = context->regs[Z80_I];
			break;
		case 'f':
			if (name[2] != 'f' || name[3] < '1' || name[4]) {
				return 0;
			}
			if (name[3] == '1') {
				*out = context->iff1;
			} else if (name[3] == '2') {
				*out = context->iff2;
			} else {
				return 0;
			}
			break;
		case 'm':
			if (name[2]) {
				return 0;
			}
			*out = context->im;
			break;
		case 'n':
			if (strcasecmp(name +2, "t_cycle")) {
				return 0;
			}
			*out = context->int_cycle;
			break;
		case 'r':
			if (name[2]) {
				return 0;
			}
			*out = context->regs[Z80_I] << 8 | context->regs[Z80_R];
			break;
		case 'x':
			switch (tolower(name[2]))
			{
			case 0:
				*out = context->regs[Z80_IXH] << 8 | context->regs[Z80_IXL];
				break;
			case 'h':
				if (name[3]) {
					return 0;
				}
				*out = context->regs[Z80_IXH];
			case 'l':
				if (name[3]) {
					return 0;
				}
				*out = context->regs[Z80_IXL];
			default:
				return 0;
			}
			break;
		case 'y':
			switch (tolower(name[2]))
			{
			case 0:
				*out = context->regs[Z80_IYH] << 8 | context->regs[Z80_IYL];
				break;
			case 'h':
				if (name[3]) {
					return 0;
				}
				*out = context->regs[Z80_IYH];
			case 'l':
				if (name[3]) {
					return 0;
				}
				*out = context->regs[Z80_IYL];
			default:
				return 0;
			}
			break;
		default:
			return 0;
		}
		break;
	case 'l':
		if (!name[1]) {
			*out = context->regs[Z80_L];
		} else if (name[1] == '\'' && !name[2]) {
			*out = context->alt_regs[Z80_L];
		} else {
			return 0;
		}
		break;
	case 'p':
		if (tolower(name[1]) != 'c' || name[2]) {
			return 0;
		}
		*out = root->address;
		break;
	case 'r':
		if (name[1]) {
			return 0;
		}
		*out = context->regs[Z80_R];
	case 's':
		if (tolower(name[1]) != 'p' || name[2]) {
			return 0;
		}
		*out = context->sp;
		break;
	default:
		return 0;
	}
	return 1;
}

static uint8_t set_z80(debug_root *root, const char *name, uint32_t value)
{
	z80_context *context = root->cpu_context;
	switch (tolower(name[0]))
	{
	case 'a':
		if (!name[1]) {
			context->regs[Z80_A] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_A] = value;
		} else if (tolower(name[1]) == 'f') {
			if (!name[2]) {
				context->regs[Z80_A] = value >> 8;
				context->flags[ZF_S] = value >> 7 & 1;
				context->flags[ZF_Z] = value >> 6 & 1;
				context->flags[ZF_H] = value >> 4 & 1;
				context->flags[ZF_PV] = value >> 2 & 1;
				context->flags[ZF_N] = value >> 1 & 1;
				context->flags[ZF_C] = value & 1;
			} else if (name[2] == '\'' && !name[3]) {
				context->alt_regs[Z80_A] = value >> 8;
				context->alt_flags[ZF_S] = value >> 7 & 1;
				context->alt_flags[ZF_Z] = value >> 6 & 1;
				context->alt_flags[ZF_H] = value >> 4 & 1;
				context->alt_flags[ZF_PV] = value >> 2 & 1;
				context->alt_flags[ZF_N] = value >> 1 & 1;
				context->alt_flags[ZF_C] = value & 1;
			} else {
				return 0;
			}
		} else {
			return 0;
		}
		break;
	case 'b':
		if (!name[1]) {
			context->regs[Z80_B] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_B] = value;
		} else if (tolower(name[1]) == 'c') {
			if (!name[2]) {
				context->regs[Z80_B] = value >> 8;
				context->regs[Z80_C] = value;
			} else if (name[2] == '\'' && !name[3]) {
				context->alt_regs[Z80_B] = value >> 8;
				context->alt_regs[Z80_C] = value;
			} else {
				return 0;
			}
		}
		break;
	case 'c':
		if (!name[1]) {
			context->regs[Z80_C] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_C] = value;
		} else {
			return 0;
		}
		break;
	case 'd':
		if (!name[1]) {
			context->regs[Z80_D] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_D] = value;
		} else if (tolower(name[1]) == 'e') {
			if (!name[2]) {
				context->regs[Z80_D] = value >> 8;
				context->regs[Z80_E] = value;
			} else if (name[2] == '\'' && !name[3]) {
				context->alt_regs[Z80_D] = value >> 8;
				context->alt_regs[Z80_E] = value;
			} else {
				return 0;
			}
		}
		break;
	case 'e':
		if (!name[1]) {
			context->regs[Z80_E] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_E] = value;
		} else {
			return 0;
		}
		break;
	case 'f':
		if (!name[1]) {
			context->flags[ZF_S] = value >> 7 & 1;
			context->flags[ZF_Z] = value >> 6 & 1;
			context->flags[ZF_H] = value >> 4 & 1;
			context->flags[ZF_PV] = value >> 2 & 1;
			context->flags[ZF_N] = value >> 1 & 1;
			context->flags[ZF_C] = value & 1;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_flags[ZF_S] = value >> 7 & 1;
			context->alt_flags[ZF_Z] = value >> 6 & 1;
			context->alt_flags[ZF_H] = value >> 4 & 1;
			context->alt_flags[ZF_PV] = value >> 2 & 1;
			context->alt_flags[ZF_N] = value >> 1 & 1;
			context->alt_flags[ZF_C] = value & 1;
		} else {
			return 0;
		}
		break;
	case 'h':
		if (!name[1]) {
			context->regs[Z80_H] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_H] = value;
		} else if (tolower(name[1]) == 'e') {
			if (!name[2]) {
				context->regs[Z80_H] = value >> 8;
				context->regs[Z80_L] = value;
			} else if (name[2] == '\'' && !name[3]) {
				context->alt_regs[Z80_H] = value >> 8;
				context->alt_regs[Z80_L] = value;
			} else {
				return 0;
			}
		}
		break;
	case 'i':
		switch (tolower(name[1]))
		{
		case 0:
			context->regs[Z80_I] = value;
			break;
		case 'f':
			if (name[2] != 'f' || name[3] < '1' || name[4]) {
				return 0;
			}
			if (name[3] == '1') {
				context->iff1 = value != 0;
			} else if (name[3] == '2') {
				context->iff2 = value != 0;
			} else {
				return 0;
			}
			break;
		case 'm':
			if (name[2]) {
				return 0;
			}
			context->im = value & 3;
			break;
		case 'r':
			if (name[2]) {
				return 0;
			}
			context->regs[Z80_I] = value >> 8;
			context->regs[Z80_R] = value;
			break;
		case 'x':
			switch (tolower(name[2]))
			{
			case 0:
				context->regs[Z80_IXH] = value >> 8;
				context->regs[Z80_IXL] = value;
				break;
			case 'h':
				if (name[3]) {
					return 0;
				}
				context->regs[Z80_IXH] = value;
			case 'l':
				if (name[3]) {
					return 0;
				}
				context->regs[Z80_IXL] = value;
			default:
				return 0;
			}
			break;
		case 'y':
			switch (tolower(name[2]))
			{
			case 0:
				context->regs[Z80_IYH] = value >> 8;
				context->regs[Z80_IYL] = value;
				break;
			case 'h':
				if (name[3]) {
					return 0;
				}
				context->regs[Z80_IYH] = value;
			case 'l':
				if (name[3]) {
					return 0;
				}
				context->regs[Z80_IYL] = value;
			default:
				return 0;
			}
			break;
		default:
			return 0;
		}
		break;
	case 'l':
		if (!name[1]) {
			context->regs[Z80_L] = value;
		} else if (name[1] == '\'' && !name[2]) {
			context->alt_regs[Z80_L] = value;
		} else {
			return 0;
		}
		break;
	case 'r':
		if (name[1]) {
			return 0;
		}
		context->regs[Z80_R] = value;
	case 's':
		if (tolower(name[1]) != 'p' || name[2]) {
			return 0;
		}
		context->sp = value;
		break;
	default:
		return 0;
	}
	return 1;
}

debug_root *find_z80_root(z80_context *context)
{
	debug_root *root = find_root(context);
	if (root && !root->commands) {
		add_commands(root, common_commands, NUM_COMMON);
		add_commands(root, z80_commands, NUM_Z80);
		if (current_system->type == SYSTEM_GENESIS || current_system->type == SYSTEM_SEGACD) {
			add_commands(root, gen_z80_commands, NUM_GEN_Z80);
		}
		root->read_mem = read_z80;
		root->write_mem = write_z80;
		root->set = set_z80;
		root->resolve = resolve_z80;
	}
	return root;
}

z80_context * zdebugger(z80_context * context, uint16_t address)
{
	static char last_cmd[1024];
	char input_buf[1024];
	z80inst inst;
	genesis_context *system = context->system;
	init_terminal();
	debug_root *root = find_z80_root(context);
	if (!root) {
		return context;
	}
	root->address = address;
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address);
	if (*this_bp) {
		if ((*this_bp)->condition) {
			uint32_t condres;
			if (eval_expr(root, (*this_bp)->condition, &condres)) {
				if (!condres) {
					return context;
				}
			} else {
				fprintf(stderr, "Failed to eval condition for Z80 breakpoint %u\n", (*this_bp)->index);
				free_expr((*this_bp)->condition);
				(*this_bp)->condition = NULL;
			}
		}
		printf("Z80 Breakpoint %d hit\n", (*this_bp)->index);
	} else {
		zremove_breakpoint(context, address);
	}
	uint8_t * pc = get_native_pointer(address, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
	if (!pc) {
		fatal_error("Failed to get native pointer on entering Z80 debugger at address %X\n", address);
	}
	uint8_t * after_pc = z80_decode(pc, &inst);
	uint16_t after = address + (after_pc-pc);
	root->after = after;
	root->inst = &inst;
	for (disp_def * cur = root->displays; cur; cur = cur->next) {
		char format_str[8];
		make_format_str(format_str, cur->format);
		for (int i = 0; i < cur->num_args; i++)
		{
			eval_expr(root, cur->args[i].parsed, &cur->args[i].value);
			do_print(root, format_str, cur->args[i].raw, cur->args[i].value);
		}
	}

	z80_disasm(&inst, input_buf, address);
	printf("%X:\t%s\n", address, input_buf);
	debugger_repl(root);
	return context;
}

#endif

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
	debug_root *root = find_m68k_root(context);
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

	root->address = address;
	int debugging = 1;
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address);
	if (*this_bp) {
		if ((*this_bp)->condition) {
			uint32_t condres;
			if (eval_expr(root, (*this_bp)->condition, &condres)) {
				if (!condres) {
					return;
				}
			} else {
				fprintf(stderr, "Failed to eval condition for M68K breakpoint %u\n", (*this_bp)->index);
				free_expr((*this_bp)->condition);
				(*this_bp)->condition = NULL;
			}
		}
		for (uint32_t i = 0; debugging && i < (*this_bp)->num_commands; i++)
		{
			debugging = run_command(root, (*this_bp)->commands + i);
		}
		if (debugging) {
			printf("68K Breakpoint %d hit\n", (*this_bp)->index);
		} else {
			return;
		}
	} else {
		remove_breakpoint(context, address);
	}
	uint32_t after = m68k_decode(m68k_instruction_fetch, context, &inst, address);
	root->after = after;
	root->inst = &inst;
	for (disp_def * cur = root->displays; cur; cur = cur->next) {
		char format_str[8];
		make_format_str(format_str, cur->format);
		for (int i = 0; i < cur->num_args; i++)
		{
			eval_expr(root, cur->args[i].parsed, &cur->args[i].value);
			do_print(root, format_str, cur->args[i].raw, cur->args[i].value);
		}
	}
	m68k_disasm(&inst, input_buf);
	printf("%X: %s\n", address, input_buf);
	debugger_repl(root);
	return;
}
