#ifndef UTIL_H_
#define UTIL_H_

#include <stdio.h>
#include <time.h>

typedef struct {
	char    *name;
	uint8_t is_dir;
} dir_entry;

//Utility functions

//Allocates a new string containing the concatenation of first and second
char * alloc_concat(char const * first, char const * second);
//Allocates a new string containing the concatenation of the strings pointed to by parts
char * alloc_concat_m(int num_parts, char const ** parts);
//Returns the size of a file using fseek and ftell
long file_size(FILE * f);
//Strips whitespace and non-printable characters from the beginning and end of a string
char * strip_ws(char * text);
//Inserts a null after the first word, returns a pointer to the second word
char * split_keyval(char * text);
//Returns the basename of a path with th extension (if any) stripped
char * basename_no_extension(char *path);
//Gets the smallest power of two that is >= a certain value, won't work for values > 0x80000000
uint32_t nearest_pow2(uint32_t val);
//Should be called by main with the value of argv[0] for use by get_exe_dir
void set_exe_str(char * str);
//Returns the directory the executable is in
char * get_exe_dir();
//Returns the user's home directory
char * get_home_dir();
//Returns an appropriate path for storing config files
char const *get_config_dir();
//Returns an appropriate path for saving non-config data like savestates
char const *get_save_dir();
//Reads a file bundled with the executable
char *read_bundled_file(char *name, long *sizeret);
//Retunrs an array of normal files and directories residing in a directory
dir_entry *get_dir_list(char *path, size_t *numret);
//Frees a dir list returned by get_dir_list
void free_dir_list(dir_entry *list, size_t numentries);
//Gets the modification time of a file
time_t get_modification_time(char *path);
//Recusrively creates a directory if it does not exist
int ensure_dir_exists(char *path);
//Returns the contents of a symlink in a newly allocated string
char * readlink_alloc(char * path);
//Prints an error message to stderr and to a message box if not in headless mode and then exits
void fatal_error(char *format, ...);
//Prints an information message to stdout and to a message box if not in headless mode and not attached to a console
void info_message(char *format, ...);
//Prints an information message to stderr and to a message box if not in headless mode and not attached to a console
void warning(char *format, ...);

#endif //UTIL_H_
