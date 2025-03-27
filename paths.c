#include <string.h>
#include <stdlib.h>
#include "blastem.h"
#include "util.h"
#include "config.h"
#include "paths.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <errno.h>
#endif
#ifdef __ANDROID__
#include <SDL_system.h>
#include <jni.h>
#endif

static char **current_path;

static char *sticky_path_path(void)
{
	if (is_config_in_exe_dir(config)) {
		return path_append(get_exe_dir(), "sticky_path");
	} else {
		return path_append(get_config_dir(), "sticky_path");
	}
}

static void persist_path(void)
{
	char *pathfname = sticky_path_path();
	FILE *f = fopen(pathfname, "wb");
	if (f) {
		if (fwrite(*current_path, 1, strlen(*current_path), f) != strlen(*current_path)) {
			warning("Failed to save menu path");
		}
		fclose(f);
	} else {
		warning("Failed to save menu path: Could not open %s for writing\n", pathfname);
		
	}
	free(pathfname);
}

#ifdef __ANDROID__
static char *get_external_storage_path()
{
	static char *ret;
	if (ret) {
		return ret;
	}
	JNIEnv *env = SDL_AndroidGetJNIEnv();
	if ((*env)->PushLocalFrame(env, 8) < 0) {
		return NULL;
	}

	jclass Environment = (*env)->FindClass(env, "android/os/Environment");
	jmethodID getExternalStorageDirectory =
		(*env)->GetStaticMethodID(env, Environment, "getExternalStorageDirectory", "()Ljava/io/File;");
	jobject file = (*env)->CallStaticObjectMethod(env, Environment, getExternalStorageDirectory);
	if (!file) {
		goto cleanup;
	}

	jmethodID getAbsolutePath = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, file),
		"getAbsolutePath", "()Ljava/lang/String;");
	jstring path = (*env)->CallObjectMethod(env, file, getAbsolutePath);

	char const *tmp = (*env)->GetStringUTFChars(env, path, NULL);
	ret = strdup(tmp);
	(*env)->ReleaseStringUTFChars(env, path, tmp);

cleanup:
	(*env)->PopLocalFrame(env, NULL);
	return ret;
}
#endif

uint8_t get_initial_browse_path(char **dst)
{
	char *base = NULL;
#ifdef __ANDROID__
	static const char activity_class_name[] = "com/retrodev/blastem/BlastEmActivity";
	static const char get_rom_path_name[] = "getRomPath";
	JNIEnv *env = SDL_AndroidGetJNIEnv();
	jclass act_class = (*env)->FindClass(env, activity_class_name);
	if (!act_class) {
		fatal_error("Failed to find activity class %s\n", activity_class_name);
	}
	jmethodID meth = (*env)->GetMethodID(env, act_class, get_rom_path_name, "()Ljava/lang/String;");
	if (!meth) {
		fatal_error("Failed to find method %s\n", get_rom_path_name);
	}
	jobject activity = SDL_AndroidGetActivity();
	jobject ret = (*env)->CallObjectMethod(env, activity, meth);
	char *res = NULL;
	if (ret) {
		const char*utf = (*env)->GetStringUTFChars(env, (jstring)ret, NULL);
		jsize len = (*env)->GetStringUTFLength(env, (jstring)ret);
		res = calloc(len + 1, 1);
		memcpy(res, utf, len);
		debug_message("Got initial browse path: %s\n", res);
		(*env)->ReleaseStringUTFChars(env, (jstring)ret, utf);
		(*env)->DeleteLocalRef(env, ret);
	}
	
	(*env)->DeleteLocalRef(env, activity);
	if (res) {
		*dst = res;
		return 1;
	}
	return 0;
#else
	char *remember_path = tern_find_path(config, "ui\0remember_path\0", TVAL_PTR).ptrval;
	if (!remember_path || !strcmp("on", remember_path)) {
		char *pathfname = sticky_path_path();
		FILE *f = fopen(pathfname, "rb");
		if (f) {
			long pathsize = file_size(f);
			if (pathsize > 0) {
				base = malloc(pathsize + 1);
				if (fread(base, 1, pathsize, f) != pathsize) {
					warning("Error restoring saved file browser path");
					free(base);
					base = NULL;
				} else {
					base[pathsize] = 0;
				}
			}
			fclose(f);
		}
		free(pathfname);
		if (!current_path) {
			atexit(persist_path);
			current_path = dst;
		}
	}
	if (!base) {
		base = tern_find_path(config, "ui\0initial_path\0", TVAL_PTR).ptrval;
	}
#endif
	if (!base){
#ifdef __ANDROID__
		
		base = get_external_storage_path();
#else
		base = "$HOME";
#endif
	}
	tern_node *vars = tern_insert_ptr(NULL, "HOME", get_home_dir());
	vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
	*dst = replace_vars(base, vars, 1);
	free(base);
	tern_free(vars);
	return 1;
}

char *path_append(const char *base, const char *suffix)
{
	if (!strcmp(suffix, "..")) {
#ifdef _WIN32
		//handle transition from root of a drive to virtual root
		if (base[1] == ':' && !base[2]) {
			return strdup(PATH_SEP);
		}
#endif
		size_t len = strlen(base);
		while (len > 0) {
			--len;
			if (is_path_sep(base[len])) {
				if (!len) {
					//special handling for /
					len++;
				}
				char *ret = malloc(len+1);
				memcpy(ret, base, len);
				ret[len] = 0;
				return ret;
			}
		}
		return strdup(PATH_SEP);
	} else {
#ifdef _WIN32
		if (base[0] == PATH_SEP[0] && !base[1]) {
			//handle transition from virtual root to root of a drive
			return strdup(suffix);
		}
#endif
		if (is_path_sep(base[strlen(base) - 1])) {
			return alloc_concat(base, suffix);
		} else {
			char const *pieces[] = {base, PATH_SEP, suffix};
			return alloc_concat_m(3, pieces);
		}
	}
}

char *path_current_dir(void)
{
	size_t size = 128;
	char *res = malloc(size);
	for (;;) {
#ifdef _WIN32
		DWORD actual = GetCurrentDirectoryA(size, res);
		if (actual > size) {
			res = realloc(res, actual);
			size = actual;
		} else {
			return res;
		}
#else
		errno = 0;
		char *tmp = getcwd(res, size);
		if (!tmp) {
			if (errno == ERANGE) {
				size *= 2;
				res = realloc(res, size);
			} else {
				free(res);
				return NULL;
			}
		} else {
			return res;
		}
#endif
	}
}
