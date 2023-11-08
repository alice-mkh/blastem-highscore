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
#ifndef NO_Z80
#include "sms.h"
#endif

#ifdef NEW_CORE
#define Z80_OPTS opts
#else
#define Z80_OPTS options
#endif

static debug_root *roots;
static uint32_t num_roots, root_storage;

debug_root *find_root(void *cpu)
{
	for (uint32_t i = 0; i < num_roots; i++)
	{
		if (roots[i].cpu_context == cpu) {
			return roots + i;
		}
	}
	if (num_roots == root_storage) {
		root_storage = root_storage ? root_storage * 2 : 5;
		roots = realloc(roots, root_storage * sizeof(debug_root));
	}
	num_roots++;
	memset(roots + num_roots - 1, 0, sizeof(debug_root));
	roots[num_roots-1].cpu_context = cpu;
	return roots + num_roots - 1;
}

bp_def ** find_breakpoint(bp_def ** cur, uint32_t address, uint8_t type)
{
	while (*cur) {
		if ((*cur)->type == type && (*cur)->address == (((*cur)->mask) & address)) {
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

static debug_array *arrays;
static uint32_t num_arrays, array_storage;
static debug_array *alloc_array(void)
{
	if (num_arrays == array_storage) {
		array_storage = array_storage ? array_storage * 2 : 4;
		arrays = realloc(arrays, sizeof(debug_array) * array_storage);
	}
	return arrays + num_arrays++;
}

static debug_val new_fixed_array(void *base, debug_array_get get, debug_array_set set, uint32_t size)
{
	debug_array *array = alloc_array();
	array->get = get;
	array->set = set;
	array->append = NULL;
	array->base = base;
	array->size = array->storage = size;
	debug_val ret;
	ret.type = DBG_VAL_ARRAY;
	ret.v.u32 = array - arrays;
	return ret;
}

static debug_val user_array_get(debug_array *array, uint32_t index)
{
	debug_val *data = array->base;
	return data[index];
}

static void user_array_set(debug_array *array, uint32_t index, debug_val value)
{
	debug_val *data = array->base;
	data[index] = value;
}

static void user_array_append(debug_array *array, debug_val value)
{
	if (array->size == array->storage) {
		array->storage *= 2;
		array->base = realloc(array->base, sizeof(debug_val) * array->storage);
	}
	debug_val *data = array->base;
	data[array->size++] = value;
}

static debug_val new_user_array(uint32_t size)
{
	debug_array *array = alloc_array();
	array->get = user_array_get;
	array->set = user_array_set;
	array->append = user_array_append;
	array->size = size;
	array->storage = size ? size : 4;
	debug_val ret;
	ret.type = DBG_VAL_ARRAY;
	ret.v.u32 = array - arrays;
	return ret;
}

debug_array *get_array(debug_val val)
{
	if (val.type != DBG_VAL_ARRAY) {
		return NULL;
	}
	return arrays + val.v.u32;
}

debug_val user_var_get(debug_var *var)
{
	return var->val;
}

void user_var_set(debug_var *var, debug_val val)
{
	var->val = val;
}

static void new_user_variable(debug_root *root, const char *name, debug_val val)
{
	debug_var *var = calloc(1, sizeof(debug_var));
	var->get = user_var_get;
	var->set = user_var_set;
	var->val = val;
	root->variables = tern_insert_ptr(root->variables, name, var);
}

static void new_readonly_variable(debug_root *root, const char *name, debug_val val)
{
	debug_var *var = calloc(1, sizeof(debug_var));
	var->get = user_var_get;
	var->set = NULL;
	var->val = val;
	root->variables = tern_insert_ptr(root->variables, name, var);
}

static debug_val debug_int(uint32_t i)
{
	debug_val ret;
	ret.type = DBG_VAL_U32;
	ret.v.u32 = i;
	return ret;
}

static debug_val debug_float(float f)
{
	return (debug_val) {
		.type = DBG_VAL_F32,
		.v = {
			.f32 = f
		}
	};
}

static const char *token_type_names[] = {
	"TOKEN_NONE",
	"TOKEN_INT",
	"TOKEN_DECIMAL",
	"TOKEN_NAME",
	"TOKEN_ARRAY",
	"TOKEN_FUNCALL",
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
			.type = TOKEN_INT,
			.v = {
				.num = strtol(start + (*start == '$' ? 1 : 2), end, 16)
			}
		};
	}
	if (isdigit(*start)) {
		uint32_t ipart = strtol(start, end, 10);
		if (**end == '.') {
			start = *end + 1;
			uint32_t fpart = strtol(start, end, 10);
			float fval;
			if (fpart) {
				float divisor = powf(10.0f, *end - start);
				fval = ipart + fpart / divisor;
			} else {
				fval = ipart;
			}
			return (token) {
				.type = TOKEN_DECIMAL,
				.v = {
					.f = fval
				}
			};
		} else {
			return (token) {
				.type = TOKEN_INT,
				.v = {
					.num = ipart
				}
			};
		}
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
	token_type type = TOKEN_NAME;
	while (**end && !isspace(**end))
	{
		uint8_t done = 0;
		switch (**end)
		{
		case '[':
			type = TOKEN_ARRAY;
			done = 1;
			break;
		case '(':
			type = TOKEN_FUNCALL;
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
		case ']':
		case ')':
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
		.type = type,
		.v = {
			.str = name
		}
	};
}

static void free_expr(expr *e);
static void free_expr_int(expr *e)
{
	free_expr(e->left);
	if (e->type == EXPR_FUNCALL) {
		for (uint32_t i = 0; i < e->op.v.num; i++)
		{
			free_expr_int(e->right + i);
		}
		free(e->right);
	} else {
		free_expr(e->right);
	}
	if (e->op.type == TOKEN_NAME) {
		free(e->op.v.str);
	}
}

static void free_expr(expr *e)
{
	if (!e) {
		return;
	}
	free_expr_int(e);
	free(e);
}

static expr *parse_scalar_or_muldiv(char *start, char **end);
static expr *parse_expression(char *start, char **end);

static void handle_namespace(expr *e)
{
	if (e->op.type != TOKEN_NAME && e->op.type != TOKEN_ARRAY) {
		return;
	}
	char *start = e->op.v.str;
	char *orig_start = start;
	for (char *cur = start; *cur; ++cur)
	{
		if (*cur == ':') {
			char *ns = malloc(cur - start + 1);
			memcpy(ns, start, cur - start);
			ns[cur - start] = 0;
			expr *inner = calloc(1, sizeof(expr));
			inner->type = EXPR_SCALAR;
			inner->op.type = TOKEN_NAME;
			start = cur + 1;
			inner->op.v.str = start;
			e->left = inner;
			e->type = EXPR_NAMESPACE;
			e->op.v.str = ns;
			e = inner;
		}
	}
	if (start != orig_start) {
		//We've split the original string up into
		//a bunch of individually allocated fragments
		//this is just a little stup of the original
		e->op.v.str = strdup(e->op.v.str);
		free(orig_start);
	}
}

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
	if (first.type == TOKEN_LBRACKET || first.type == TOKEN_ARRAY) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		if (first.type == TOKEN_ARRAY) {
			//current token is the array name
			//consume the bracket token
			parse_token(after_first, &after_first);
			ret->right = calloc(1, sizeof(expr));
			ret->right->type = EXPR_SCALAR;
			ret->right->op = first;
			handle_namespace(ret->right);
		}

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
	if (first.type == TOKEN_FUNCALL) {
		expr *ret = calloc(1, sizeof(expr));
		ret->left = calloc(1, sizeof(expr));
		ret->left->type = EXPR_SCALAR;
		ret->left->op = first;
		ret->left->op.type = TOKEN_NAME;
		uint32_t storage = 0;
		ret->op.v.num = 0;
		token next = parse_token(after_first, end);
		while (next.type != TOKEN_RPAREN && next.type != TOKEN_NONE)
		{
			expr *e = parse_expression(after_first, end);
			if (!e) {
				fprintf(stderr, "Expression expected after '('\n");
				free_expr(ret);
				return NULL;
			}
			if (storage == ret->op.v.num) {
				storage = storage ? storage * 2 : 1;
				ret->right = realloc(ret->right, storage * sizeof(expr));
			}
			ret->right[ret->op.v.num++] = *e;
			free(e);
			after_first = *end;
			next = parse_token(after_first, end);
		}
		if (next.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing ')' after '('\n");
			free_expr(ret);
			return NULL;
		}
		return ret;
	}
	if (first.type != TOKEN_INT && first.type != TOKEN_DECIMAL && first.type != TOKEN_NAME) {
		fprintf(stderr, "Unexpected token %s\n", token_type_names[first.type]);
		return NULL;
	}
	token second = parse_token(after_first, end);
	if (second.type != TOKEN_SIZE) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		handle_namespace(ret);
		*end = after_first;
		return ret;
	}
	expr *ret = calloc(1, sizeof(expr));
	ret->type = EXPR_SIZE;
	ret->left = calloc(1, sizeof(expr));
	ret->left->type = EXPR_SCALAR;
	ret->left->op = second;
	handle_namespace(ret->left);
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
	if (first.type == TOKEN_LBRACKET || first.type == TOKEN_ARRAY) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		if (first.type == TOKEN_ARRAY) {
			//current token is the array name
			//consume the bracket token
			parse_token(after_first, &after_first);
			ret->right = calloc(1, sizeof(expr));
			ret->right->type = EXPR_SCALAR;
			ret->right->op = first;
			handle_namespace(ret->right);
		}

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
	if (first.type == TOKEN_FUNCALL) {
		expr *ret = calloc(1, sizeof(expr));
		ret->left = calloc(1, sizeof(expr));
		ret->left->type = EXPR_SCALAR;
		ret->left->op = first;
		ret->left->op.type = TOKEN_NAME;
		uint32_t storage = 0;
		ret->op.v.num = 0;
		token next = parse_token(after_first, end);
		while (next.type != TOKEN_RPAREN && next.type != TOKEN_NONE)
		{
			expr *e = parse_expression(after_first, end);
			if (!e) {
				fprintf(stderr, "Expression expected after '('\n");
				free_expr(ret);
				return NULL;
			}
			if (storage == ret->op.v.num) {
				storage = storage ? storage * 2 : 1;
				ret->right = realloc(ret->right, storage * sizeof(expr));
			}
			ret->right[ret->op.v.num++] = *e;
			free(e);
			after_first = *end;
			next = parse_token(after_first, end);
		}
		if (next.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing ')' after '('\n");
			free_expr(ret);
			return NULL;
		}
		return maybe_muldiv(ret, *end, end);
	}
	if (first.type != TOKEN_INT && first.type != TOKEN_DECIMAL && first.type != TOKEN_NAME) {
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
		handle_namespace(bin->left);
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
		handle_namespace(value->left);
		return maybe_muldiv(value, after_second, end);
	} else {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		handle_namespace(ret);
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
	if (first.type == TOKEN_LBRACKET || first.type == TOKEN_ARRAY) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_MEM;
		if (first.type == TOKEN_ARRAY) {
			//current token is the array name
			//consume the bracket token
			parse_token(after_first, &after_first);
			ret->right = calloc(1, sizeof(expr));
			ret->right->type = EXPR_SCALAR;
			ret->right->op = first;
			handle_namespace(ret->right);
		}

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
	if (first.type == TOKEN_FUNCALL) {
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_FUNCALL;
		ret->left = calloc(1, sizeof(expr));
		ret->left->type = EXPR_SCALAR;
		ret->left->op = first;
		ret->left->op.type = TOKEN_NAME;
		uint32_t storage = 0;
		ret->op.v.num = 0;
		//consume LPAREN
		parse_token(after_first, end);
		after_first = *end;
		token next = parse_token(after_first, end);
		while (next.type != TOKEN_RPAREN && next.type != TOKEN_NONE)
		{
			expr *e = parse_expression(after_first, end);
			if (!e) {
				fprintf(stderr, "Expression expected after '('\n");
				free_expr(ret);
				return NULL;
			}
			if (storage == ret->op.v.num) {
				storage = storage ? storage * 2 : 1;
				ret->right = realloc(ret->right, storage * sizeof(expr));
			}
			ret->right[ret->op.v.num++] = *e;
			free(e);
			after_first = *end;
			next = parse_token(after_first, end);
		}
		if (next.type != TOKEN_RPAREN) {
			fprintf(stderr, "Missing ')' after '('\n");
			free_expr(ret);
			return NULL;
		}
		return maybe_binary(ret, *end, end);
	}
	if (first.type != TOKEN_INT && first.type != TOKEN_DECIMAL && first.type != TOKEN_NAME) {
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
		handle_namespace(bin->left);
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
		handle_namespace(value->left);
		return maybe_binary(value, after_second, end);
	} else {
		if (second.type == TOKEN_NAME) {
			free(second.v.str);
		}
		expr *ret = calloc(1, sizeof(expr));
		ret->type = EXPR_SCALAR;
		ret->op = first;
		handle_namespace(ret);
		*end = after_first;
		return ret;
	}
}

uint8_t eval_expr(debug_root *root, expr *e, debug_val *out)
{
	debug_val right;
	switch(e->type)
	{
	case EXPR_SCALAR:
		if (e->op.type == TOKEN_NAME || e->op.type == TOKEN_ARRAY) {
			debug_var *var = tern_find_ptr(root->variables, e->op.v.str);
			if (!var) {
				return 0;
			}
			*out = var->get(var);
			return 1;
		} else if (e->op.type == TOKEN_INT) {
			*out = debug_int(e->op.v.num);
			return 1;
		} else {
			*out = debug_float(e->op.v.f);
			return 1;
		}
	case EXPR_UNARY:
		if (!eval_expr(root, e->left, out)) {
			return 0;
		}
		switch (e->op.v.op[0])
		{
		case '!':
			if (out->type != DBG_VAL_U32) { fprintf(stderr, "operator ! is only defined for integers"); return 0; }
			out->v.u32 = !out->v.u32;
			break;
		case '~':
			if (out->type != DBG_VAL_U32) { fprintf(stderr, "operator ~ is only defined for integers"); return 0; }
			out->v.u32 = ~out->v.u32;
			break;
		case '-':
			if (out->type == DBG_VAL_U32) {
				out->v.u32 = -out->v.u32;
			} else if (out->type == DBG_VAL_F32) {
				out->v.f32 = -out->v.f32;
			} else {
				fprintf(stderr, "operator ~ is only defined for integers and floats");
				return 0;
			}
			break;
		default:
			return 0;
		}
		return 1;
	case EXPR_BINARY:
		if (!eval_expr(root, e->left, out) || !eval_expr(root, e->right, &right)) {
			return 0;
		}
		if (out->type != right.type) {
			if (out->type == DBG_VAL_F32) {
				if (right.type == DBG_VAL_U32) {
					right.type = DBG_VAL_F32;
					float v = right.v.u32;
					right.v.f32 = v;
				} else {
					fprintf(stderr, "Invalid type on right side of binary operator\n");
					return 0;
				}
			} else if (out->type == DBG_VAL_U32) {
				if (right.type == DBG_VAL_F32) {
					out->type = DBG_VAL_F32;
					float v = out->v.u32;
					out->v.f32 = v;
				} else {
					fprintf(stderr, "Invalid type on right side of binary operator\n");
					return 0;
				}
			}
		}
		if (out->type == DBG_VAL_U32) {
			switch (e->op.v.op[0])
			{
			case '+':
				out->v.u32 += right.v.u32;
				break;
			case '-':
				out->v.u32 -= right.v.u32;
				break;
			case '*':
				out->v.u32 *= right.v.u32;
				break;
			case '/':
				out->v.u32 /= right.v.u32;
				break;
			case '&':
				out->v.u32 &= right.v.u32;
				break;
			case '|':
				out->v.u32 |= right.v.u32;
				break;
			case '^':
				out->v.u32 ^= right.v.u32;
				break;
			case '=':
				out->v.u32 = out->v.u32 == right.v.u32;
				break;
			case '!':
				out->v.u32 = out->v.u32 != right.v.u32;
				break;
			case '>':
				out->v.u32 = e->op.v.op[1] ? out->v.u32 >= right.v.u32 : out->v.u32 > right.v.u32;
				break;
			case '<':
				out->v.u32 = e->op.v.op[1] ? out->v.u32 <= right.v.u32 : out->v.u32 < right.v.u32;
				break;
			default:
				return 0;
			}
		} else if (out->type == DBG_VAL_F32) {
			switch (e->op.v.op[0])
			{
			case '+':
				out->v.f32 += right.v.f32;
				break;
			case '-':
				out->v.f32 -= right.v.f32;
				break;
			case '*':
				out->v.f32 *= right.v.f32;
				break;
			case '/':
				out->v.f32 /= right.v.f32;
				break;
			case '=':
				out->v.u32 = out->v.f32 == right.v.f32;
				out->type = DBG_VAL_U32;
				break;
			case '!':
				out->v.u32 = out->v.f32 != right.v.f32;
				out->type = DBG_VAL_U32;
				break;
			case '>':
				out->v.u32 = e->op.v.op[1] ? out->v.f32 >= right.v.f32 : out->v.f32 > right.v.f32;
				out->type = DBG_VAL_U32;
				break;
			case '<':
				out->v.u32 = e->op.v.op[1] ? out->v.f32 <= right.v.f32 : out->v.f32 < right.v.f32;
				out->type = DBG_VAL_U32;
				break;
			default:
				return 0;
			}
		}
		return 1;
	case EXPR_SIZE:
		if (!eval_expr(root, e->left, out)) {
			return 0;
		}
		if (out->type != DBG_VAL_U32) { fprintf(stderr, "Size expressions are only defined for integers"); return 0; }
		switch (e->op.v.op[0])
		{
		case 'b':
			out->v.u32 &= 0xFF;
			break;
		case 'w':
			out->v.u32 &= 0xFFFF;
			break;
		}
		return 1;
	case EXPR_MEM:
		if (!eval_expr(root, e->left, out)) {
			return 0;
		}
		if (out->type != DBG_VAL_U32) { fprintf(stderr, "Array index must be integer"); return 0; }
		if (e->right) {
			if (!eval_expr(root, e->right, &right)) {
				return 0;
			}
			debug_array *array = get_array(right);
			if (!array) {
				fprintf(stderr, "Attempt to index into value that is not an array");
				return 0;
			}
			if (out->v.u32 >= array->size) {
				return 0;
			}
			*out = array->get(array, out->v.u32);
			return 1;
		}
		return root->read_mem(root, &out->v.u32, e->op.v.op[0]);
	case EXPR_NAMESPACE:
		root = tern_find_ptr(root->other_roots, e->op.v.str);
		if (!root) {
			fprintf(stderr, "%s is not a valid namespace\n", e->op.v.str);
			return 0;
		}
		return eval_expr(root, e->left, out);
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

static uint8_t debug_cast_int(debug_val val, uint32_t *out)
{
	if (val.type == DBG_VAL_U32) {
		*out = val.v.u32;
		return 1;
	}
	if (val.type == DBG_VAL_F32) {
		*out = val.v.f32;
		return 1;
	}
	return 0;
}

static uint8_t debug_cast_float(debug_val val, float *out)
{
	if (val.type == DBG_VAL_U32) {
		*out = val.v.u32;
		return 1;
	}
	if (val.type == DBG_VAL_F32) {
		*out = val.v.f32;
		return 1;
	}
	return 0;
}

static uint8_t debug_cast_bool(debug_val val)
{
	switch(val.type)
	{
	case DBG_VAL_U32: return val.v.u32 != 0;
	case DBG_VAL_F32: return val.v.f32 != 0.0f;
	case DBG_VAL_ARRAY: return get_array(val)->size != 0;
	default: return 1;
	}
}

static debug_val m68k_dreg_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	return debug_int(context->dregs[var->val.v.u32]);
}

static void m68k_dreg_set(debug_var *var, debug_val val)
{
	m68k_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "M68K register d%d can only be set to an integer\n", var->val.v.u32);
		return;
	}
	context->dregs[var->val.v.u32] = ival;
}

static debug_val m68k_areg_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	return debug_int(context->aregs[var->val.v.u32]);
}

static void m68k_areg_set(debug_var *var, debug_val val)
{
	m68k_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "M68K register a%d can only be set to an integer\n", var->val.v.u32);
		return;
	}
	context->aregs[var->val.v.u32] = ival;
}

static debug_val m68k_sr_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	debug_val ret;
	ret.v.u32 = context->status << 8;
	for (int flag = 0; flag < 5; flag++)
	{
		ret.v.u32 |= context->flags[flag] << (4-flag);
	}
	ret.type = DBG_VAL_U32;
	return ret;
}

static void m68k_sr_set(debug_var *var, debug_val val)
{
	m68k_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "M68K register sr can only be set to an integer\n");
		return;
	}
	context->status = ival >> 8;
	for (int flag = 0; flag < 5; flag++) {
		context->flags[flag] = (ival & (1 << (4 - flag))) != 0;
	}
}

static debug_val m68k_cycle_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	return debug_int(context->current_cycle);
}

static debug_val m68k_usp_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	return debug_int(context->status & 0x20 ? context->aregs[8] : context->aregs[7]);
}

static void m68k_usp_set(debug_var *var, debug_val val)
{
	m68k_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "M68K register usp can only be set to an integer\n");
		return;
	}
	context->aregs[context->status & 0x20 ? 8 : 7] = ival;
}

static debug_val m68k_ssp_get(debug_var *var)
{
	m68k_context *context = var->ptr;
	return debug_int(context->status & 0x20 ? context->aregs[7] : context->aregs[8]);
}

static void m68k_ssp_set(debug_var *var, debug_val val)
{
	m68k_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "M68K register ssp can only be set to an integer\n");
		return;
	}
	context->aregs[context->status & 0x20 ? 7 : 8] = ival;
}

static debug_val root_address_get(debug_var *var)
{
	debug_root *root = var->ptr;
	return debug_int(root->address);
}

static void m68k_names(debug_root *root)
{
	debug_var *var;
	for (char i = 0; i < 8; i++)
	{
		char rname[3] = {'d', '0' + i, 0};
		var = calloc(1, sizeof(debug_var));
		var->get = m68k_dreg_get;
		var->set = m68k_dreg_set;
		var->ptr = root->cpu_context;
		var->val.v.u32 = i;
		root->variables = tern_insert_ptr(root->variables, rname, var);
		rname[0] = 'D';
		root->variables = tern_insert_ptr(root->variables, rname, var);

		var = calloc(1, sizeof(debug_var));
		var->get = m68k_areg_get;
		var->set = m68k_areg_set;
		var->ptr = root->cpu_context;
		var->val.v.u32 = i;
		rname[0] = 'a';
		root->variables = tern_insert_ptr(root->variables, rname, var);
		rname[0] = 'A';
		root->variables = tern_insert_ptr(root->variables, rname, var);
		if (i == 7) {
			root->variables = tern_insert_ptr(root->variables, "sp", var);
			root->variables = tern_insert_ptr(root->variables, "SP", var);
		}
	}

	var = calloc(1, sizeof(debug_var));
	var->get = m68k_sr_get;
	var->set = m68k_sr_set;
	var->ptr = root->cpu_context;
	root->variables = tern_insert_ptr(root->variables, "sr", var);
	root->variables = tern_insert_ptr(root->variables, "SR", var);

	var = calloc(1, sizeof(debug_var));
	var->get = root_address_get;
	var->ptr = root;
	root->variables = tern_insert_ptr(root->variables, "pc", var);
	root->variables = tern_insert_ptr(root->variables, "PC", var);

	var = calloc(1, sizeof(debug_var));
	var->get = m68k_usp_get;
	var->set = m68k_usp_set;
	var->ptr = root->cpu_context;
	root->variables = tern_insert_ptr(root->variables, "usp", var);
	root->variables = tern_insert_ptr(root->variables, "USP", var);

	var = calloc(1, sizeof(debug_var));
	var->get = m68k_ssp_get;
	var->set = m68k_ssp_set;
	var->ptr = root->cpu_context;
	root->variables = tern_insert_ptr(root->variables, "ssp", var);
	root->variables = tern_insert_ptr(root->variables, "SSP", var);

	var = calloc(1, sizeof(debug_var));
	var->get = m68k_cycle_get;
	var->ptr = root->cpu_context;
	root->variables = tern_insert_ptr(root->variables, "cycle", var);
}

static debug_val vcounter_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp->vcounter);
}

static debug_val hcounter_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp->hslot);
}

static debug_val vdp_address_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp->address);
}

static void vdp_address_set(debug_var *var, debug_val val)
{
	vdp_context *vdp = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "vdp address can only be set to an integer\n");
		return;
	}
	vdp->address = ival & 0x1FFFF;
}

static debug_val vdp_cd_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp->cd);
}

static void vdp_cd_set(debug_var *var, debug_val val)
{
	vdp_context *vdp = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "vdp cd can only be set to an integer\n");
		return;
	}
	vdp->cd = ival & 0x3F;
}

static debug_val vdp_status_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp_status(vdp));
}

static debug_val debug_vram_get(debug_array *array, uint32_t index)
{
	vdp_context *vdp = array->base;
	return debug_int(vdp->vdpmem[index]);
}

static void debug_vram_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "vram can only be set to integers\n");
		return;
	}
	vdp_context *vdp = array->base;
	vdp->vdpmem[index] = ival;
}

static debug_val debug_vsram_get(debug_array *array, uint32_t index)
{
	vdp_context *vdp = array->base;
	return debug_int(vdp->vsram[index] & VSRAM_BITS);
}

static void debug_vsram_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "vsram can only be set to integers\n");
		return;
	}
	vdp_context *vdp = array->base;
	vdp->vsram[index] = ival;
}

static debug_val debug_cram_get(debug_array *array, uint32_t index)
{
	vdp_context *vdp = array->base;
	return debug_int(vdp->cram[index] & CRAM_BITS);
}

static void debug_cram_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "cram can only be set to integers\n");
		return;
	}
	vdp_context *vdp = array->base;
	vdp->cram[index] = ival;
}

static debug_val debug_vreg_get(debug_array *array, uint32_t index)
{
	vdp_context *vdp = array->base;
	return debug_int(vdp->regs[index]);
}

static void debug_vreg_set(debug_array *array, uint32_t index, debug_val val)
{
	vdp_context *vdp = array->base;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "vdp registers can only be set to integers\n");
		return;
	}
	vdp_reg_write(vdp, index, ival);
}

static debug_root* find_vdp_root(vdp_context *context)
{
	debug_root *root = find_root(context);
	debug_var *var = calloc(1, sizeof(debug_var));
	var->get = vcounter_get;
	var->ptr = context;
	root->variables = tern_insert_ptr(root->variables, "vcounter", var);

	var = calloc(1, sizeof(debug_var));
	var->get = hcounter_get;
	var->ptr = context;
	root->variables = tern_insert_ptr(root->variables, "hcounter", var);

	var = calloc(1, sizeof(debug_var));
	var->get = vdp_address_get;
	var->set = vdp_address_set;
	var->ptr = context;
	root->variables = tern_insert_ptr(root->variables, "address", var);

	var = calloc(1, sizeof(debug_var));
	var->get = vdp_cd_get;
	var->set = vdp_cd_set;
	var->ptr = context;
	root->variables = tern_insert_ptr(root->variables, "cd", var);

	new_readonly_variable(root, "vram", new_fixed_array(context, debug_vram_get, debug_vram_set, VRAM_SIZE));
	new_readonly_variable(root, "vsram", new_fixed_array(context, debug_vsram_get, debug_vsram_set, context->vsram_size));
	new_readonly_variable(root, "cram", new_fixed_array(context, debug_cram_get, debug_cram_set, CRAM_SIZE));
	new_readonly_variable(root, "reg", new_fixed_array(context, debug_vreg_get, debug_vreg_set, VDP_REGS));

	return root;
}

static debug_val debug_part1_get(debug_array *array, uint32_t index)
{
	ym2612_context *ym = array->base;
	return debug_int(ym->part1_regs[index]);
}

static void debug_part1_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "ym2612 registers can only be set to integers\n");
		return;
	}
	ym2612_context *ym = array->base;
	uint8_t old_part = ym->selected_part;
	uint8_t old_reg = ym->selected_reg;
	ym->selected_part = 0;
	ym->selected_reg = index;
	ym_data_write(ym, ival);
	ym->selected_part = old_part;
	ym->selected_reg = old_reg;
}

static debug_val debug_part2_get(debug_array *array, uint32_t index)
{
	ym2612_context *ym = array->base;
	return debug_int(ym->part2_regs[index]);
}

static void debug_part2_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "ym2612 registers can only be set to integers\n");
		return;
	}
	ym2612_context *ym = array->base;
	uint8_t old_part = ym->selected_part;
	uint8_t old_reg = ym->selected_reg;
	ym->selected_part = 1;
	ym->selected_reg = index;
	ym_data_write(ym, ival);
	ym->selected_part = old_part;
	ym->selected_reg = old_reg;
}

static debug_root* find_ym2612_root(ym2612_context *context)
{
	debug_root *root = find_root(context);

	new_readonly_variable(root, "part1", new_fixed_array(context, debug_part1_get, debug_part1_set, YM_REG_END));
	new_readonly_variable(root, "part2", new_fixed_array(context, debug_part2_get, debug_part2_set, YM_REG_END));

	return root;
}


static debug_val debug_psgfreq_get(debug_array *array, uint32_t index)
{
	psg_context *psg = array->base;
	return debug_int(psg->counter_load[index]);
}

static void debug_psgfreq_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "psg registers can only be set to integers\n");
		return;
	}
	psg_context *psg = array->base;
	psg->counter_load[index] = ival;
}

static debug_val debug_psgcount_get(debug_array *array, uint32_t index)
{
	psg_context *psg = array->base;
	return debug_int(psg->counters[index]);
}

static void debug_psgcount_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "psg registers can only be set to integers\n");
		return;
	}
	psg_context *psg = array->base;
	psg->counters[index] = ival;
}

static debug_val debug_psgvol_get(debug_array *array, uint32_t index)
{
	psg_context *psg = array->base;
	return debug_int(psg->volume[index]);
}

static void debug_psgvol_set(debug_array *array, uint32_t index, debug_val val)
{
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "psg registers can only be set to integers\n");
		return;
	}
	psg_context *psg = array->base;
	psg->volume[index] = ival;
}

debug_root *find_psg_root(psg_context *psg)
{
	debug_root *root = find_root(psg);

	new_readonly_variable(root, "frequency", new_fixed_array(psg, debug_psgfreq_get, debug_psgfreq_set, 4));
	new_readonly_variable(root, "counter", new_fixed_array(psg, debug_psgcount_get, debug_psgcount_set, 4));
	new_readonly_variable(root, "volume", new_fixed_array(psg, debug_psgvol_get, debug_psgvol_set, 4));

	return root;
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
#ifndef IS_LIB
		render_update_display();
#endif
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

static void do_print(debug_root *root, char *format_str, char *raw, debug_val value)
{
	switch(value.type)
	{
	case DBG_VAL_U32:
		if (format_str[5] == 's') {
			char tmp[128];
			int j;
			uint32_t addr = value.v.u32;
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
			printf(format_str, raw, value.v.u32);
		}
		break;
	case DBG_VAL_F32: {
		char tmp = format_str[5];
		format_str[5] = 'f';
		printf(format_str, raw, value.v.f32);
		format_str[5] = tmp;
		break;
	}
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
			case 'f':
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
			debug_val val;
			if (!eval_expr(root, arg, &val)) {
				free(fmt);
				return 1;
			}
			if (cur[1] == 's') {
				if (val.type == DBG_VAL_STRING) {
					//TODO: implement me
				} else {
					char tmp[128];
					uint32_t address;
					if (!debug_cast_int(val, &address)) {
						fprintf(stderr, "Format char 's' accepts only integers and strings\n");
						free(fmt);
						return 1;
					}
					int j;
					for (j = 0; j < sizeof(tmp)-1; j++, address++)
					{
						uint32_t addr = address;
						root->read_mem(root, &addr, 'b');
						char c = addr;
						if (c < 0x20 || c > 0x7F) {
							break;
						}
						tmp[j] = c;
					}
					tmp[j] = 0;
					printf(format_str, tmp);
				}
			} else if (cur[1] == 'f') {
				float fval;
				if (!debug_cast_float(val, &fval)) {
					fprintf(stderr, "Format char '%c' only accepts floats\n", cur[1]);
					free(fmt);
					return 1;
				}
				printf(format_str, fval);
			} else {
				uint32_t ival;
				if (!debug_cast_int(val, &ival)) {
					fprintf(stderr, "Format char '%c' only accepts integers\n", cur[1]);
					free(fmt);
					return 1;
				}
				printf(format_str, ival);
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
	uint32_t index;
	if (!debug_cast_int(cmd->args[0].value, &index)) {
		fprintf(stderr, "Argument to deletedisplay must be an integer\n");
		return 1;
	}
	while (*cur)
	{
		if ((*cur)->index == index) {
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
	uint32_t index;
	if (!debug_cast_int(cmd->args[0].value, &index)) {
		fprintf(stderr, "Argument to commands must be an integer\n");
		return 1;
	}
	bp_def **target = find_breakpoint_idx(&root->breakpoints, index);
	if (!target) {
		fprintf(stderr, "Breakpoint %d does not exist!\n", index);
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
	return execute_block(root, debug_cast_bool(cmd->args[0].value) ? &cmd->block : &cmd->else_block);
}

static uint8_t cmd_while(debug_root *root, parsed_command *cmd)
{
	if (!debug_cast_bool(cmd->args[0].value)) {
		return execute_block(root, &cmd->else_block);
	}
	int debugging = 1;
	do {
		debugging = execute_block(root, &cmd->block) && debugging;
		if (!eval_expr(root, cmd->args[0].parsed, &cmd->args[0].value)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[0].raw);
			return 1;
		}
	} while (debug_cast_bool(cmd->args[0].value));
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
	debug_root *set_root = root;
	expr *set_expr = cmd->args[0].parsed;
	while (set_expr->type == EXPR_NAMESPACE)
	{
		set_root = tern_find_ptr(set_root->other_roots, set_expr->op.v.str);
		if (!set_root) {
			fprintf(stderr, "%s is not a valid namespace\n", set_expr->op.v.str);
			return 1;
		}
		set_expr = set_expr->left;
	}
	debug_val address;
	debug_array *array = NULL;
	switch (set_expr->type)
	{
	case EXPR_SCALAR:
		if (set_expr->op.type == TOKEN_NAME) {
			name = set_expr->op.v.str;
		} else {
			fputs("First argument to set must be a name or memory expression, not a number", stderr);
			return 1;
		}
		break;
	case EXPR_SIZE:
		size = set_expr->op.v.op[0];
		if (set_expr->left->op.type == TOKEN_NAME) {
			name = set_expr->left->op.v.str;
		} else {
			fputs("First argument to set must be a name or memory expression, not a number", stderr);
			return 1;
		}
		break;
	case EXPR_MEM:
		size = set_expr->op.v.op[0];
		if (!eval_expr(root, set_expr->left, &address)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[0].raw);
			return 1;
		}
		if (address.type != DBG_VAL_U32) {
			fprintf(stderr, "Index in array expression must be integer\n");
			return 1;
		}
		if (set_expr->right) {
			debug_val right;
			if (!eval_expr(root, set_expr->right, &right)) {
				return 1;
			}
			array = get_array(right);
			if (!array) {
				fprintf(stderr, "%s does not refer to an array\n", cmd->args[0].raw);
				return 1;
			}
			if (!array->set) {
				fprintf(stderr, "Array %s is read-only\n", set_expr->right->op.v.str);
				return 1;
			}
			if (address.v.u32 >= array->size) {
				fprintf(stderr, "Address %X is out of bounds for array %s\n", address.v.u32, set_expr->right->op.v.str);
				return 1;
			}
		}
		break;
	default:
		fprintf(stderr, "First argument to set must be a name or memory expression, got %s\n", expr_type_names[set_expr->type]);
		return 1;
	}
	if (!eval_expr(root, cmd->args[1].parsed, &cmd->args[1].value)) {
		fprintf(stderr, "Failed to eval %s\n", cmd->args[1].raw);
		return 1;
	}
	debug_val value = cmd->args[1].value;
	if (name) {
		debug_var *var = tern_find_ptr(set_root->variables, name);
		if (!var) {
			fprintf(stderr, "%s is not defined\n", name);
			return 1;
		}
		if (!var->set) {
			fprintf(stderr, "%s is read-only\n", name);
			return 1;
		}
		if (size && size != 'l') {
			debug_val old = var->get(var);
			if (size == 'b') {
				old.v.u32 &= 0xFFFFFF00;
				value.v.u32 &= 0xFF;
				value.v.u32 |= old.v.u32;
			} else {
				old.v.u32 &= 0xFFFF0000;
				value.v.u32 &= 0xFFFF;
				value.v.u32 |= old.v.u32;
			}
		}
		var->set(var, value);
	} else if (array) {
		array->set(array, address.v.u32, value);
	} else if (!root->write_mem(root, address.v.u32, value.v.u32, size)) {
		fprintf(stderr, "Failed to write to address %X\n", address.v.u32);
	}
	return 1;
}

static uint8_t cmd_variable(debug_root *root, parsed_command *cmd)
{
	debug_root *set_root = root;
	expr *set_expr = cmd->args[0].parsed;
	while (set_expr->type == EXPR_NAMESPACE)
	{
		set_root = tern_find_ptr(set_root->other_roots, set_expr->op.v.str);
		if (!set_root) {
			fprintf(stderr, "%s is not a valid namespace\n", set_expr->op.v.str);
			return 1;
		}
		set_expr = set_expr->left;
	}
	if (set_expr->type != EXPR_SCALAR || set_expr->op.type != TOKEN_NAME) {
		fprintf(stderr, "First argument to variable must be a name, got %s\n", expr_type_names[set_expr->type]);
		return 1;
	}
	debug_var *var = tern_find_ptr(set_root->variables, set_expr->op.v.str);
	if (var) {
		fprintf(stderr, "%s is already defined\n", set_expr->op.v.str);
		return 1;
	}
	debug_val value;
	value.type = DBG_VAL_U32;
	value.v.u32 = 0;
	if (cmd->num_args > 1) {
		if (!eval_expr(root, cmd->args[1].parsed, &cmd->args[1].value)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[1].raw);
			return 1;
		}
		value = cmd->args[1].value;
	}
	new_user_variable(set_root, set_expr->op.v.str, value);
	return 1;
}

static uint8_t cmd_array(debug_root *root, parsed_command *cmd)
{
	debug_root *set_root = root;
	expr *set_expr = cmd->args[0].parsed;
	while (set_expr->type == EXPR_NAMESPACE)
	{
		set_root = tern_find_ptr(set_root->other_roots, set_expr->op.v.str);
		if (!set_root) {
			fprintf(stderr, "%s is not a valid namespace\n", set_expr->op.v.str);
			return 1;
		}
		set_expr = set_expr->left;
	}
	if (set_expr->type != EXPR_SCALAR || set_expr->op.type != TOKEN_NAME) {
		fprintf(stderr, "First argument to array must be a name, got %s\n", expr_type_names[set_expr->type]);
		return 1;
	}
	debug_var *var = tern_find_ptr(set_root->variables, set_expr->op.v.str);
	debug_array *array;
	if (var) {
		debug_val val = var->get(var);
		array = get_array(val);
		if (!array) {
			fprintf(stderr, "%s is already defined as a non-array value\n", set_expr->op.v.str);
			return 1;
		}
	} else {
		var = calloc(1, sizeof(debug_var));
		var->get = user_var_get;
		var->set = user_var_set;
		var->val = new_user_array(cmd->num_args - 1);
		set_root->variables = tern_insert_ptr(set_root->variables, set_expr->op.v.str, var);
		array = get_array(var->val);
	}
	if (array->set == user_array_set) {
		array->size = cmd->num_args - 1;
		if (array->storage < array->size) {
			array->storage = array->size;
			array->base = realloc(array->base, sizeof(debug_val) * array->storage);
		}
	}
	for (uint32_t i = 1; i < cmd->num_args && i < array->size; i++)
	{
		if (!eval_expr(root, cmd->args[i].parsed, &cmd->args[i].value)) {
			fprintf(stderr, "Failed to eval %s\n", cmd->args[i].raw);
			return 1;
		}
		array->set(array, i - 1, cmd->args[i].value);
	}
	return 1;
}

static uint8_t cmd_append(debug_root *root, parsed_command *cmd)
{
	debug_array *array = get_array(cmd->args[0].value);
	if (!array) {
		fprintf(stderr, "%s is not an array\n", cmd->args[0].raw);
		return 1;
	}
	if (!array->append) {
		fprintf(stderr, "Array %s doesn't support appending\n", cmd->args[0].raw);
		return 1;
	}
	array->append(array, cmd->args[1].value);
	return 1;
}

static uint8_t cmd_frames(debug_root *root, parsed_command *cmd)
{
	uint32_t frames;
	if (!debug_cast_int(cmd->args[0].value, &frames)) {
		fprintf(stderr, "Argument to frames must be an integer\n");
		return 1;
	}
	current_system->enter_debugger_frames = frames;
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
	uint32_t index;
	if (!debug_cast_int(cmd->args[0].value, &index)) {
		fprintf(stderr, "First argument to condition must be an integer\n");
		return 1;
	}
	bp_def **target = find_breakpoint_idx(&root->breakpoints, index);
	if (!*target) {
		fprintf(stderr, "Failed to find breakpoint %u\n", index);
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

static void symbol_max_len(char *key, tern_val val, uint8_t valtype, void *data)
{
	size_t *max_len = data;
	size_t len = strlen(key);
	if (len > *max_len) {
		*max_len = len;
	}
}

static void print_symbol(char *key, tern_val val, uint8_t valtype, void *data)
{
	size_t *padding = data;
	size_t len = strlen(key);
	fputs(key, stdout);
	while (len < *padding)
	{
		putchar(' ');
		len++;
	}
	printf("%X\n", (uint32_t)val.intval);
}

static uint8_t cmd_symbols(debug_root *root, parsed_command *cmd)
{
	char *filename = cmd->raw ? strip_ws(cmd->raw) : NULL;
	if (filename && *filename) {
		FILE *f = fopen(filename, "r");
		if (!f) {
			fprintf(stderr, "Failed to open %s for reading\n", filename);
			return 1;
		}
		char linebuf[1024];
		while (fgets(linebuf, sizeof(linebuf), f))
		{
			char *line = strip_ws(linebuf);
			if (*line) {
				char *end;
				uint32_t address = strtol(line, &end, 16);
				if (end != line) {
					if (*end == '=') {
						char *name = strip_ws(end + 1);
						add_label(root->disasm, name, address);
						root->symbols = tern_insert_int(root->symbols, name, address);
					}
				}
			}
		}
	} else {
		size_t max_len = 0;
		tern_foreach(root->symbols, symbol_max_len, &max_len);
		max_len += 2;
		tern_foreach(root->symbols, print_symbol, &max_len);
	}
	return 1;
}

static uint8_t cmd_delete_m68k(debug_root *root, parsed_command *cmd)
{
	uint32_t index;
	if (!debug_cast_int(cmd->args[0].value, &index)) {
		fprintf(stderr, "Argument to delete must be an integer\n");
		return 1;
	}
	bp_def **this_bp = find_breakpoint_idx(&root->breakpoints, index);
	if (!*this_bp) {
		fprintf(stderr, "Breakpoint %d does not exist\n", index);
		return 1;
	}
	bp_def *tmp = *this_bp;
	if (tmp->type == BP_TYPE_CPU) {
		remove_breakpoint(root->cpu_context, tmp->address);
	}
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
	uint32_t address;
	if (!debug_cast_int(cmd->args[0].value, &address)) {
		fprintf(stderr, "Argument to breakpoint must be an integer\n");
		return 1;
	}
	insert_breakpoint(root->cpu_context, address, debugger);
	bp_def *new_bp = calloc(1, sizeof(bp_def));
	new_bp->next = root->breakpoints;
	new_bp->address = address;
	new_bp->mask = 0xFFFFFF;
	new_bp->index = root->bp_index++;
	new_bp->type = BP_TYPE_CPU;
	root->breakpoints = new_bp;
	printf("68K Breakpoint %d set at %X\n", new_bp->index, address);
	return 1;
}

static void on_vdp_reg_write(vdp_context *context, uint16_t reg, uint16_t value)
{
	value &= 0xFF;
	if (context->regs[reg] == value) {
		return;
	}
	genesis_context *gen = (genesis_context *)context->system;
	debug_root *root = find_m68k_root(gen->m68k);
	bp_def **this_bp = find_breakpoint(&root->breakpoints, reg, BP_TYPE_VDPREG);
	int debugging = 1;
	if (*this_bp) {
		if ((*this_bp)->condition) {
			debug_val condres;
			if (eval_expr(root, (*this_bp)->condition, &condres)) {
				if (!condres.v.u32) {
					return;
				}
			} else {
				fprintf(stderr, "Failed to eval condition for VDP Register Breakpoint %u\n", (*this_bp)->index);
				free_expr((*this_bp)->condition);
				(*this_bp)->condition = NULL;
			}
		}
		for (uint32_t i = 0; debugging && i < (*this_bp)->num_commands; i++)
		{
			debugging = run_command(root, (*this_bp)->commands + i);
		}
		if (debugging) {
			printf("VDP Register Breakpoint %d hit on register write %X - Old: %X, New: %X\n", (*this_bp)->index, reg, context->regs[reg], value);
			gen->header.enter_debugger = 1;
			if (gen->m68k->sync_cycle > gen->m68k->current_cycle + 1) {
				gen->m68k->sync_cycle = gen->m68k->current_cycle + 1;
			}
			if (gen->m68k->target_cycle > gen->m68k->sync_cycle) {
				gen->m68k->target_cycle = gen->m68k->sync_cycle;
			}
		}
	}
}

static uint8_t cmd_vdp_reg_break(debug_root *root, parsed_command *cmd)
{
	bp_def *new_bp = calloc(1, sizeof(bp_def));
	new_bp->next = root->breakpoints;
	if (cmd->num_args) {
		if (!debug_cast_int(cmd->args[0].value, &new_bp->address)) {
			fprintf(stderr, "Arguments to vdpregbreak must be integers if provided\n");
			return 1;
		}
		if (cmd->num_args > 1) {
			if (!debug_cast_int(cmd->args[1].value, &new_bp->mask)) {
				fprintf(stderr, "Arguments to vdpregbreak must be integers if provided\n");
				return 1;
			}
		} else {
			new_bp->mask = 0xFF;
		}
	}
	new_bp->index = root->bp_index++;
	new_bp->type = BP_TYPE_VDPREG;
	root->breakpoints = new_bp;
	m68k_context *m68k = root->cpu_context;
	genesis_context *gen = m68k->system;
	gen->vdp->reg_hook = on_vdp_reg_write;
	printf("VDP Register Breakpoint %d set\n", new_bp->index);
	return 1;
}

static uint8_t cmd_advance_m68k(debug_root *root, parsed_command *cmd)
{
	uint32_t address;
	if (!debug_cast_int(cmd->args[0].value, &address)) {
		fprintf(stderr, "Argument to advance must be an integer\n");
		return 1;
	}
	insert_breakpoint(root->cpu_context, address, debugger);
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

static uint8_t cmd_disassemble_m68k(debug_root *root, parsed_command *cmd)
{
	m68k_context *context = root->cpu_context;
	uint32_t address = root->address;
	if (cmd->num_args) {
		if (!debug_cast_int(cmd->args[0].value, &address)) {
			fprintf(stderr, "Argument to disassemble must be an integer if provided\n");
			return 1;
		}
	}
	char disasm_buf[1024];
	m68kinst inst;
	do {
		label_def *def = find_label(root->disasm, address);
		if (def) {
			for (uint32_t i = 0; i < def->num_labels; i++)
			{
				printf("%s:\n", def->labels[i]);
			}
		}

		address = m68k_decode(m68k_instruction_fetch, context, &inst, address);
		m68k_disasm_labels(&inst, disasm_buf, root->disasm);
		printf("\t%s\n", disasm_buf);
	} while(!m68k_is_terminal(&inst));
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
		ym_print_channel_info(gen->ym, cmd->args[0].value.v.u32 - 1);
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
			"variable", NULL
		},
		.usage = "variable NAME [VALUE]",
		.desc = "Create a new variable called NAME and set it to VALUE or 0 if no value provided",
		.impl = cmd_variable,
		.min_args = 1,
		.max_args = 2,
		.skip_eval = 1
	},
	{
		.names = (const char *[]){
			"array", NULL
		},
		.usage = "array NAME [VALUE...]",
		.desc = "Create a new array called NAME if it doesn't already exist. The array is initialized with the remaining parameters",
		.impl = cmd_array,
		.min_args = 1,
		.max_args = -1,
		.skip_eval = 1
	},
	{
		.names = (const char *[]){
			"append", NULL
		},
		.usage = "append NAME VALUE",
		.desc = "Increase the size of array NAME by 1 and set the last element to VALUE",
		.impl = cmd_append,
		.min_args = 2,
		.max_args = 2
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
	},
	{
		.names = (const char *[]){
			"symbols", NULL
		},
		.usage = "symbols [FILENAME]",
		.desc = "Loads a list of symbols from the file indicated by FILENAME or lists currently loaded symbols if FILENAME is omitted",
		.impl = cmd_symbols,
		.min_args = 0,
		.max_args = 1,
		.raw_args = 1
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
	},
	{
		.names = (const char *[]){
			"disassemble", "disasm", NULL
		},
		.usage = "disassemble [ADDRESS]",
		.desc = "Disassemble code starting at ADDRESS if provided or the current address if not",
		.impl = cmd_disassemble_m68k,
		.min_args = 0,
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
			"vdpregs", "vr", NULL
		},
		.usage = "vdpregs",
		.desc = "Print VDP register values with a short description",
		.impl = cmd_vdp_regs,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"vdpregbreak", "vregbreak", "vrb", NULL
		},
		.usage = "vdpregbreak [REGISTER [MASK]]",
		.desc = "Enter debugger on VDP register write. If REGISTER is provided, breakpoint will only fire for writes to that register. If MASK is also provided, it will be applied to the register number before comparison with REGISTER",
		.impl = cmd_vdp_reg_break,
		.min_args = 0,
		.max_args = 2
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
	uint32_t index;
	if (!debug_cast_int(cmd->args[0].value, &index)) {
		fprintf(stderr, "Argument to delete must be an integer\n");
		return 1;
	}
	bp_def **this_bp = find_breakpoint_idx(&root->breakpoints, index);
	if (!*this_bp) {
		fprintf(stderr, "Breakpoint %d does not exist\n", index);
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
	uint32_t address;
	if (!debug_cast_int(cmd->args[0].value, &address)) {
		fprintf(stderr, "Argument to breakpoint must be an integer\n");
		return 1;
	}
	zinsert_breakpoint(root->cpu_context, address, (uint8_t *)zdebugger);
	bp_def *new_bp = calloc(1, sizeof(bp_def));
	new_bp->next = root->breakpoints;
	new_bp->address = address;
	new_bp->mask = 0xFFFF;
	new_bp->type = BP_TYPE_CPU;
	new_bp->index = root->bp_index++;
	root->breakpoints = new_bp;
	printf("Z80 Breakpoint %d set at %X\n", new_bp->index, address);
	return 1;
}

static uint8_t cmd_advance_z80(debug_root *root, parsed_command *cmd)
{
	uint32_t address;
	if (!debug_cast_int(cmd->args[0].value, &address)) {
		fprintf(stderr, "Argument to advance must be an integer\n");
		return 1;
	}
	zinsert_breakpoint(root->cpu_context, address, (uint8_t *)zdebugger);
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

static uint8_t cmd_disassemble_z80(debug_root *root, parsed_command *cmd)
{
	z80_context *context = root->cpu_context;
	uint32_t address = root->address;
	if (cmd->num_args) {
		if (!debug_cast_int(cmd->args[0].value, &address)) {
			fprintf(stderr, "Argument to disassemble must be an integer if provided\n");
			return 1;
		}
	}
	char disasm_buf[1024];
	z80inst inst;
	do {
		label_def *def = find_label(root->disasm, address);
		if (def) {
			for (uint32_t i = 0; i < def->num_labels; i++)
			{
				printf("%s:\n", def->labels[i]);
			}
		}
		uint8_t *pc = get_native_pointer(address, (void **)context->mem_pointers, &context->Z80_OPTS->gen);
		uint8_t *after = z80_decode(pc, &inst);
		z80_disasm(&inst, disasm_buf, address);
		address += after - pc;
		printf("\t%s\n", disasm_buf);
	} while(!z80_is_terminal(&inst));
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

static uint8_t cmd_vdp_sprites_sms(debug_root *root, parsed_command *cmd)
{
	z80_context *context = root->cpu_context;
	sms_context * sms = context->system;
	vdp_print_sprite_table(sms->vdp);
	return 1;
}

static uint8_t cmd_vdp_regs_sms(debug_root *root, parsed_command *cmd)
{
	z80_context *context = root->cpu_context;
	sms_context * sms = context->system;
	vdp_print_reg_explain(sms->vdp);
	return 1;
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
	},
	{
		.names = (const char *[]){
			"disassemble", "disasm", NULL
		},
		.usage = "disassemble [ADDRESS]",
		.desc = "Disassemble code starting at ADDRESS if provided or the current address if not",
		.impl = cmd_disassemble_z80,
		.min_args = 0,
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

command_def sms_commands[] = {
	{
		.names = (const char *[]){
			"vdpsprites", "vs", NULL
		},
		.usage = "vdpsprites",
		.desc = "Print the VDP sprite table",
		.impl = cmd_vdp_sprites_sms,
		.min_args = 0,
		.max_args = 0
	},
	{
		.names = (const char *[]){
			"vdpsregs", "vr", NULL
		},
		.usage = "vdpregs",
		.desc = "Print VDP register values with a short description",
		.impl = cmd_vdp_regs_sms,
		.min_args = 0,
		.max_args = 0
	}
};

#define NUM_SMS (sizeof(sms_commands)/sizeof(*sms_commands))

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

static void symbol_map(char *key, tern_val val, uint8_t valtype, void *data)
{
	debug_root *root = data;
	label_def *label = val.ptrval;
	for (uint32_t i = 0; i < label->num_labels; i++)
	{
		root->symbols = tern_insert_int(root->symbols, label->labels[i], label->full_address);
	}
}

static debug_val debug_frame_get(debug_var *var)
{
	vdp_context *vdp = var->ptr;
	return debug_int(vdp->frame);
}

debug_root *find_m68k_root(m68k_context *context)
{
	debug_root *root = find_root(context);
	if (root && !root->commands) {
		add_commands(root, common_commands, NUM_COMMON);
		add_commands(root, m68k_commands, NUM_68K);
		root->read_mem = read_m68k;
		root->write_mem = write_m68k;
		root->disasm = create_68000_disasm();
		m68k_names(root);
		debug_var *var;
		switch (current_system->type)
		{
		case SYSTEM_GENESIS:
		case SYSTEM_SEGACD:
			//check if this is the main CPU
			if (context->system == current_system) {
				genesis_context *gen = context->system;
				root->other_roots = tern_insert_ptr(root->other_roots, "z80", find_z80_root(gen->z80));
				root->other_roots = tern_insert_ptr(root->other_roots, "vdp", find_vdp_root(gen->vdp));
				root->other_roots = tern_insert_ptr(root->other_roots, "ym", find_ym2612_root(gen->ym));
				root->other_roots = tern_insert_ptr(root->other_roots, "psg", find_psg_root(gen->psg));
				add_commands(root, genesis_commands, NUM_GENESIS);
				var = calloc(1, sizeof(debug_var));
				var->get = debug_frame_get;
				var->ptr = gen->vdp;
				root->variables = tern_insert_ptr(root->variables, "frame", var);
				if (current_system->type == SYSTEM_SEGACD) {
					add_segacd_maincpu_labels(root->disasm);
					add_commands(root, scd_main_commands, NUM_SCD_MAIN);
					segacd_context *scd = gen->expansion;
					root->other_roots = tern_insert_ptr(root->other_roots, "sub", find_m68k_root(scd->m68k));
				}
				break;
			} else {
				add_segacd_subcpu_labels(root->disasm);
				add_commands(root, scd_sub_commands, NUM_SCD_SUB);
				segacd_context *scd = context->system;
				root->other_roots = tern_insert_ptr(root->other_roots, "main", find_m68k_root(scd->genesis->m68k));
			}
		default:
			break;
		}
		tern_foreach(root->disasm->labels, symbol_map, root);
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

static debug_val z80_reg8_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->regs[var->val.v.u32]);
}

static void z80_reg8_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register %s can only be set to an integer\n", z80_regs[var->val.v.u32]);
		return;
	}
	context->regs[var->val.v.u32] = ival;
}


static debug_val z80_alt_reg8_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->alt_regs[var->val.v.u32]);
}

static void z80_alt_reg8_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register %s' can only be set to an integer\n", z80_regs[var->val.v.u32]);
		return;
	}
	context->alt_regs[var->val.v.u32] = ival;
}

static debug_val z80_flags_get(debug_var *var)
{
	z80_context *context = var->ptr;
	debug_val ret;
	ret.type = DBG_VAL_U32;
	ret.v.u32 = context->flags[ZF_S] << 7;
	ret.v.u32 |= context->flags[ZF_Z] << 6;
	ret.v.u32 |= context->flags[ZF_H] << 4;
	ret.v.u32 |= context->flags[ZF_PV] << 2;
	ret.v.u32 |= context->flags[ZF_N] << 1;
	ret.v.u32 |= context->flags[ZF_C];
	return ret;
}

static debug_val z80_alt_flags_get(debug_var *var)
{
	z80_context *context = var->ptr;
	debug_val ret;
	ret.type = DBG_VAL_U32;
	ret.v.u32 = context->alt_flags[ZF_S] << 7;
	ret.v.u32 |= context->alt_flags[ZF_Z] << 6;
	ret.v.u32 |= context->alt_flags[ZF_H] << 4;
	ret.v.u32 |= context->alt_flags[ZF_PV] << 2;
	ret.v.u32 |= context->alt_flags[ZF_N] << 1;
	ret.v.u32 |= context->alt_flags[ZF_C];
	return ret;
}

static void z80_flags_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register F can only be set to an integer\n");
		return;
	}
	context->flags[ZF_S] = ival >> 7 & 1;
	context->flags[ZF_Z] = ival >> 6 & 1;
	context->flags[ZF_H] = ival >> 4 & 1;
	context->flags[ZF_PV] = ival >> 2 & 1;
	context->flags[ZF_N] = ival >> 1 & 1;
	context->flags[ZF_C] = ival & 1;
}

static void z80_alt_flags_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register F' can only be set to an integer\n");
		return;
	}
	context->alt_flags[ZF_S] = ival >> 7 & 1;
	context->alt_flags[ZF_Z] = ival >> 6 & 1;
	context->alt_flags[ZF_H] = ival >> 4 & 1;
	context->alt_flags[ZF_PV] = ival >> 2 & 1;
	context->alt_flags[ZF_N] = ival >> 1 & 1;
	context->alt_flags[ZF_C] = ival & 1;
}

static debug_val z80_regpair_get(debug_var *var)
{
	z80_context *context = var->ptr;
	debug_val ret;
	if (var->val.v.u32 == Z80_AF) {
		ret = z80_flags_get(var);
		ret.v.u32 |= context->regs[Z80_A] << 8;
	} else {
		ret = debug_int(context->regs[z80_high_reg(var->val.v.u32)] << 8 | context->regs[z80_low_reg(var->val.v.u32)]);
	}
	return ret;
}

static void z80_regpair_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register %s can only be set to an integer\n", z80_regs[var->val.v.u32]);
		return;
	}
	if (var->val.v.u32 == Z80_AF) {
		context->regs[Z80_A] = ival >> 8;
		z80_flags_set(var, val);
	} else {
		context->regs[z80_high_reg(var->val.v.u32)] = ival >> 8;
		context->regs[z80_low_reg(var->val.v.u32)] = ival;
	}
}

static debug_val z80_alt_regpair_get(debug_var *var)
{
	z80_context *context = var->ptr;
	debug_val ret;
	if (var->val.v.u32 == Z80_AF) {
		ret = z80_alt_flags_get(var);
		ret.v.u32 |= context->alt_regs[Z80_A] << 8;
	} else {
		ret = debug_int(context->alt_regs[z80_high_reg(var->val.v.u32)] << 8 | context->alt_regs[z80_low_reg(var->val.v.u32)]);
	}
	return ret;
}

static void z80_alt_regpair_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register %s' can only be set to an integer\n", z80_regs[var->val.v.u32]);
		return;
	}
	if (var->val.v.u32 == Z80_AF) {
		context->regs[Z80_A] = ival >> 8;
		z80_alt_flags_set(var, val);
	} else {
		context->alt_regs[z80_high_reg(var->val.v.u32)] = ival >> 8;
		context->alt_regs[z80_low_reg(var->val.v.u32)] = ival;
	}
}

static debug_val z80_sp_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->sp);
}

static void z80_sp_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register sp can only be set to an integer\n");
		return;
	}
	context->sp = ival;
}

static debug_val z80_im_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->im);
}

static void z80_im_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	uint32_t ival;
	if (!debug_cast_int(val, &ival)) {
		fprintf(stderr, "Z80 register im can only be set to an integer\n");
		return;
	}
	context->im = ival & 3;
}

static debug_val z80_iff1_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->iff1);
}

static void z80_iff1_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	context->iff1 = debug_cast_bool(val);
}

static debug_val z80_iff2_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->iff2);
}

static void z80_iff2_set(debug_var *var, debug_val val)
{
	z80_context *context = var->ptr;
	context->iff2 = debug_cast_bool(val);
}

static debug_val z80_cycle_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->current_cycle);
}

static debug_val z80_pc_get(debug_var *var)
{
	z80_context *context = var->ptr;
	return debug_int(context->pc);
}

static void z80_names(debug_root *root)
{
	debug_var *var;
	for (int i = 0; i < Z80_UNUSED; i++)
	{
		var = calloc(1, sizeof(debug_var));
		var->ptr = root->cpu_context;
		if (i < Z80_BC) {
			var->get = z80_reg8_get;
			var->set = z80_reg8_set;
		} else if (i == Z80_SP) {
			var->get = z80_sp_get;
			var->set = z80_sp_set;
		} else {
			var->get = z80_regpair_get;
			var->set = z80_regpair_set;
		}
		var->val.v.u32 = i;
		root->variables = tern_insert_ptr(root->variables, z80_regs[i], var);
		size_t name_size = strlen(z80_regs[i]);
		char *name = malloc(name_size + 2);
		char *d = name;
		for (const char *c = z80_regs[i]; *c; c++, d++)
		{
			*d = toupper(*c);
		}
		name[name_size] = 0;
		root->variables = tern_insert_ptr(root->variables, name, var);

		if (i < Z80_IXL || (i > Z80_R && i < Z80_IX && i != Z80_SP)) {
			memcpy(name, z80_regs[i], name_size);
			name[name_size] = '\'';
			name[name_size + 1] = 0;
			var = calloc(1, sizeof(debug_var));
			var->ptr = root->cpu_context;
			if (i < Z80_BC) {
				var->get = z80_alt_reg8_get;
				var->set = z80_alt_reg8_set;
			} else {
				var->get = z80_alt_regpair_get;
				var->set = z80_alt_regpair_set;
			}
			var->val.v.u32 = i;
			root->variables = tern_insert_ptr(root->variables, name, var);
			d = name;
			for (const char *c = z80_regs[i]; *c; c++, d++)
			{
				*d = toupper(*c);
			}
			root->variables = tern_insert_ptr(root->variables, name, var);
		}
		free(name);
	}
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_flags_get;
	var->set = z80_flags_set;
	root->variables = tern_insert_ptr(root->variables, "f", var);
	root->variables = tern_insert_ptr(root->variables, "F", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_alt_flags_get;
	var->set = z80_alt_flags_set;
	root->variables = tern_insert_ptr(root->variables, "f'", var);
	root->variables = tern_insert_ptr(root->variables, "F'", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_im_get;
	var->set = z80_im_set;
	root->variables = tern_insert_ptr(root->variables, "im", var);
	root->variables = tern_insert_ptr(root->variables, "IM", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_iff1_get;
	var->set = z80_iff1_set;
	root->variables = tern_insert_ptr(root->variables, "iff1", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_iff2_get;
	var->set = z80_iff2_set;
	root->variables = tern_insert_ptr(root->variables, "iff2", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_cycle_get;
	root->variables = tern_insert_ptr(root->variables, "cycle", var);
	var = calloc(1, sizeof(debug_var));
	var->ptr = root->cpu_context;
	var->get = z80_pc_get;
	root->variables = tern_insert_ptr(root->variables, "pc", var);
	root->variables = tern_insert_ptr(root->variables, "PC", var);
}

debug_root *find_z80_root(z80_context *context)
{
	debug_root *root = find_root(context);
	if (root && !root->commands) {
		add_commands(root, common_commands, NUM_COMMON);
		add_commands(root, z80_commands, NUM_Z80);
		z80_names(root);
		genesis_context *gen;
		sms_context *sms;
		debug_var *var;
		//TODO: populate names
		switch (current_system->type)
		{
		case SYSTEM_GENESIS:
		case SYSTEM_SEGACD:
			gen = context->system;
			add_commands(root, gen_z80_commands, NUM_GEN_Z80);
			root->other_roots = tern_insert_ptr(root->other_roots, "m68k", find_m68k_root(gen->m68k));
			//root->resolve = resolve_z80;
			break;
		case SYSTEM_SMS:
			sms = context->system;
			add_commands(root, sms_commands, NUM_SMS);
			root->other_roots = tern_insert_ptr(root->other_roots, "vdp", find_vdp_root(sms->vdp));
			root->other_roots = tern_insert_ptr(root->other_roots, "psg", find_psg_root(sms->psg));
			var = calloc(1, sizeof(debug_var));
			var->get = debug_frame_get;
			var->ptr = sms->vdp;
			root->variables = tern_insert_ptr(root->variables, "frame", var);
			break;
		//default:
			//root->resolve = resolve_z80;
		}
		root->read_mem = read_z80;
		root->write_mem = write_z80;
		root->disasm = create_z80_disasm();
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
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address, BP_TYPE_CPU);
	if (*this_bp) {
		if ((*this_bp)->condition) {
			debug_val condres;
			if (eval_expr(root, (*this_bp)->condition, &condres)) {
				if (!condres.v.u32) {
					return context;
				}
			} else {
				fprintf(stderr, "Failed to eval condition for Z80 breakpoint %u\n", (*this_bp)->index);
				free_expr((*this_bp)->condition);
				(*this_bp)->condition = NULL;
			}
		}
		int debugging = 1;
		for (uint32_t i = 0; debugging && i < (*this_bp)->num_commands; i++)
		{
			debugging = run_command(root, (*this_bp)->commands + i);
		}
		if (debugging) {
			printf("Z80 Breakpoint %d hit\n", (*this_bp)->index);
		} else {
			return context;
		}
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
		bp_def ** f_bp = find_breakpoint(&root->breakpoints, root->branch_f, BP_TYPE_CPU);
		if (!*f_bp) {
			remove_breakpoint(context, root->branch_f);
		}
		root->branch_t = root->branch_f = 0;
	} else if(address == root->branch_f) {
		bp_def ** t_bp = find_breakpoint(&root->breakpoints, root->branch_t, BP_TYPE_CPU);
		if (!*t_bp) {
			remove_breakpoint(context, root->branch_t);
		}
		root->branch_t = root->branch_f = 0;
	}

	root->address = address;
	int debugging = 1;
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&root->breakpoints, address, BP_TYPE_CPU);
	if (*this_bp) {
		if ((*this_bp)->condition) {
			debug_val condres;
			if (eval_expr(root, (*this_bp)->condition, &condres)) {
				if (!condres.v.u32) {
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
	m68k_disasm_labels(&inst, input_buf, root->disasm);
	printf("%X: %s\n", address, input_buf);
	debugger_repl(root);
	return;
}
