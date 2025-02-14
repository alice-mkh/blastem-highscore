#disable built-in rules
.SUFFIXES :

ifndef OS
OS:=$(shell uname -s)
endif
ifndef SDL
SDL:=sdl2
endif
SDL_UPPER:=$(shell echo $(SDL) | tr '[a-z]' '[A-Z]')
FIXUP:=true
Z80_DISPATCH:=goto

BUNDLED_LIBZ:=zlib/adler32.o zlib/compress.o zlib/crc32.o zlib/deflate.o zlib/gzclose.o zlib/gzlib.o zlib/gzread.o\
	zlib/gzwrite.o zlib/infback.o zlib/inffast.o zlib/inflate.o zlib/inftrees.o zlib/trees.o zlib/uncompr.o zlib/zutil.o

ifeq ($(OS),Windows)

GLEW_PREFIX:=glew
MEM:=mem_win.o
TERMINAL:=terminal_win.o
FONT:=nuklear_ui/font_win.o
CHOOSER:=nuklear_ui/filechooser_win.o
NET:=net_win.o
EXE:=.exe
SO:=dll
CPU:=i686
ifeq ($(CPU),i686)
CC:=i686-w64-mingw32-gcc-win32
WINDRES:=i686-w64-mingw32-windres
GLUDIR:=Win32
SDL2_PREFIX:="sdl/i686-w64-mingw32"
else
CC:=x86_64-w64-mingw32-gcc-win32
WINDRES:=x86_64-w64-mingw32-windres
SDL2_PREFIX:="sdl/x86_64-w64-mingw32"
GLUDIR:=x64
endif
GLEW32S_LIB:=$(GLEW_PREFIX)/lib/Release/$(GLUDIR)/glew32s.lib
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -Wpointer-arith -Werror=pointer-arith
LDFLAGS:=-lm -lmingw32 -lws2_32 -lcomdlg32 -mwindows
ifneq ($(MAKECMDGOALS),libblastem.dll)
CFLAGS+= -I"$(SDL2_PREFIX)/include/$(SDL_UPPER)" -I"$(GLEW_PREFIX)/include" -DGLEW_STATIC
LDFLAGS+= $(GLEW32S_LIB) -L"$(SDL2_PREFIX)/lib" -l$(SDL_UPPER)main -l$(SDL_UPPER) -lopengl32 -lglu32
endif
LIBZOBJS=$(BUNDLED_LIBZ)

else

MEM:=mem.o
TERMINAL:=terminal.o
NET:=net.o
EXE:=

HAS_PROC:=$(shell if [ -d /proc ]; then /bin/echo -e -DHAS_PROC; fi)
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -Wno-unused-value  -Wpointer-arith -Werror=pointer-arith $(HAS_PROC) -DHAVE_UNISTD_H

ifeq ($(OS),Darwin)
LIBS=$(SDL) glew
FONT:=nuklear_ui/font_mac.o
CHOOSER:=nuklear_ui/filechooser_null.o
SO:=dylib
else
SO:=so

ifeq ($(CPU),wasm)
USE_GLES:=1
endif

ifdef USE_FBDEV
LIBS=alsa
ifndef NOGL
LIBS+=glesv2 egl
endif
CFLAGS+= -DUSE_GLES -DUSE_FBDEV -pthread
else
ifdef USE_GLES
LIBS=$(SDL) glesv2
CFLAGS+= -DUSE_GLES
else
LIBS=$(SDL) glew gl
endif #USE_GLES
endif #USE_FBDEV
ifeq ($(CPU),wasm)
CHOOSER:=nuklear_ui/filechooser_null.o
FONT:=nuklear_ui/font_web.o
Z80_DISPATCH:=call
else #CPU=wasm
FONT:=nuklear_ui/font.o
ifneq ($(MAKECMDGOALS),libblastem.$(SO))
CHOOSER:=nuklear_ui/filechooser_gtk.o
GTKFLAGS:=$(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
ifeq ($(GTKFLAGS),)
GTKFLAGS:=$(shell pkg-config --cflags gtk+-2.0 2>/dev/null)
ifeq ($(GTKFLAGS),)
CHOOSER:=nuklear_ui/filechooser_null.o
endif
endif
endif #neq ($(MAKECMDGOALS),libblastem.$(SO))
endif #CPU=wasm
ifeq ($(GTKFLAGS),)
else
EXTRA_NUKLEAR_LDFLAGS:=-ldl
endif
CFLAGS+= $(GTKFLAGS)
endif #Darwin

ifdef HOST_ZLIB
LIBS+= zlib
LIBZOBJS=
else
LIBZOBJS=$(BUNDLED_LIBZ)
endif

ifeq ($(OS),Darwin)
#This should really be based on whether or not the C compiler is clang rather than based on the OS
CFLAGS+= -Wno-logical-op-parentheses
endif

ifeq ($(CPU),wasm)
CFLAGS+= --use-port=sdl2
LDFLAGS+= --use-port=sdl2 --embed-file rom.db --embed-file default.cfg --embed-file systems.cfg --embed-file shaders/ --embed-file images/ --embed-file DroidSans.ttf --embed-file roms/
EXE:=.html
else #CPU=wasm

ifdef PORTABLE
ifdef USE_GLES
ifndef GLES_LIB
GLES_LIB:=$(shell pkg-config --libs glesv2)
endif
LDFLAGS:=-lm $(GLES_LIB)
else #USE_GLES
CFLAGS+= -DGLEW_STATIC -Iglew/include
LDFLAGS:=-lm glew/lib/libGLEW.a -lEGL
endif #USE_GLES

ifeq ($(OS),Darwin)
SDL_INCLUDE_PATH:=Frameworks/$(SDL_UPPER).framework/Headers
CFLAGS+=  -mmacosx-version-min=10.10
LDFLAGS+= -FFrameworks -framework $(SDL_UPPER) -framework OpenGL -framework AppKit -mmacosx-version-min=10.10
FIXUP:=install_name_tool -change @rpath/$(SDL_UPPER).framework/Versions/A/$(SDL_UPPER) @executable_path/Frameworks/$(SDL_UPPER).framework/Versions/A/$(SDL_UPPER)
else #Darwin
ifeq ($(SDL),sdl3)
SDL_INCLUDE_PATH:=sdl/include/SDL3
CFLAGS+= -Isdl/include -DSDL_ENABLE_OLD_NAMES
else
SDL_INCLUDE_PATH:=sdl/include
endif
LDFLAGS+= -Wl,-rpath='$$ORIGIN/lib' -Llib -l$(SDL_UPPER)
ifndef USE_GLES
LDFLAGS+= $(shell pkg-config --libs gl)
endif
endif #Darwin
CFLAGS+= -I$(SDL_INCLUDE_PATH)

else #PORTABLE
ifeq ($(MAKECMDGOALS),libblastem.$(SO))
LDFLAGS:=-lm
else
CFLAGS:=$(shell pkg-config --cflags-only-I $(LIBS)) $(CFLAGS)
LDFLAGS:=-lm $(shell pkg-config --libs $(LIBS))
ifdef USE_FBDEV
LDFLAGS+= -pthread
endif
endif #libblastem.so

ifeq ($(OS),Darwin)
LDFLAGS+= -framework OpenGL -framework AppKit
endif

endif #PORTABLE
endif #CPU=wasm
endif #Windows

ifdef DEBUG
OBJDIR:=obj/debug
OPT:=-g3 -O0
else
OBJDIR:=obj/release
ifdef NOLTO
OPT:=-O2
else
OPT:=-O2 -flto
endif #NOLTO
endif #DEBUG
LIBOBJDIR:=$(OBJDIR)/lib

CFLAGS:=$(OPT) $(CFLAGS)
LDFLAGS:=$(OPT) $(LDFLAGS)

ifdef Z80_LOG_ADDRESS
CFLAGS+= -DZ80_LOG_ADDRESS
endif

ifdef PROFILE
PROFFLAGS:= -Wl,--no-as-needed -lprofiler -Wl,--as-needed
CFLAGS+= -g3
endif
ifdef NOGL
CFLAGS+= -DDISABLE_OPENGL
endif

ifdef M68030
CFLAGS+= -DM68030
endif
ifdef M68020
CFLAGS+= -DM68020
endif
ifdef M68010
CFLAGS+= -DM68010
endif

ifndef CPU
CPU:=$(shell uname -m)
endif

#OpenBSD uses different names for these architectures
ifeq ($(CPU),amd64)
CPU:=x86_64
else
ifeq ($(CPU),i386)
CPU:=i686
endif
endif
ifeq ($(CPU),x86_64)
CFLAGS+=-DX86_64 -m64
LDFLAGS+=-m64
else
ifeq ($(CPU),i686)
CFLAGS+=-DX86_32 -m32
LDFLAGS+=-m32
else
NEW_CORE:=1
endif
endif

TRANSOBJS=gen.o backend.o $(MEM) arena.o tern.o
M68KOBJS=68kinst.o disasm.o

ifdef NO_FILE_CHOOSER
CHOOSER:=nuklear_ui/filechooser_nulll.o
endif

ifdef NEW_CORE
Z80OBJS=z80.o z80inst.o
M68KOBJS+= m68k.o
CFLAGS+= -DNEW_CORE
else
Z80OBJS=z80inst.o z80_to_x86.o
ifeq ($(CPU),x86_64)
M68KOBJS+= m68k_core.o m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
else
ifeq ($(CPU),i686)
M68KOBJS+= m68k_core.o m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
endif
endif
endif
AUDIOOBJS=ym2612.o ymf262.o ym_common.o psg.o wave.o flac.o vgm.o event_log.o render_audio.o rf5c164.o
CONFIGOBJS=config.o tern.o util.o paths.o
NUKLEAROBJS=$(FONT) $(CHOOSER) nuklear_ui/blastem_nuklear.o nuklear_ui/sfnt.o
RENDEROBJS=ppm.o controller_info.o
ifdef USE_FBDEV
RENDEROBJS+= render_fbdev.o
else
RENDEROBJS+= render_sdl.o
endif

ifdef NOZLIB
CFLAGS+= -DDISABLE_ZLIB
else
RENDEROBJS+= $(LIBZOBJS) png.o
endif

COREOBJS:=system.o genesis.o vdp.o io.o romdb.o hash.o xband.o realtec.o i2c.o nor.o $(M68KOBJS) \
	sega_mapper.o multi_game.o megawifi.o $(NET) serialize.o $(TERMINAL) $(CONFIGOBJS) gst.o \
	$(TRANSOBJS) $(AUDIOOBJS) saves.o jcart.o gen_player.o coleco.o pico_pcm.o ymz263b.o \
	segacd.o lc8951.o cdimage.o cdd_mcu.o cd_graphics.o cdd_fader.o sft_mapper.o mediaplayer.o

ifdef NOZ80
CFLAGS+=-DNO_Z80
else
COREOBJS+= sms.o i8255.o $(Z80OBJS)
endif

MAINOBJS:=$(COREOBJS) blastem.o $(RENDEROBJS) zip.o  menu.o debug.o gdb_remote.o bindings.o oscilloscope.o

LIBOBJS:=$(COREOBJS) libblastem.o rom.db.o $(LIBZOBJS)

ifdef NONUKLEAR
CFLAGS+= -DDISABLE_NUKLEAR
else
MAINOBJS+= $(NUKLEAROBJS)
LDFLAGS+=$(EXTRA_NUKLEAR_LDFLAGS)
endif

ifeq ($(OS),Windows)
MAINOBJS+= res.o
endif

ifdef CONFIG_PATH
CFLAGS+= -DCONFIG_PATH='"'$(CONFIG_PATH)'"'
endif

ifdef DATA_PATH
CFLAGS+= -DDATA_PATH='"'$(DATA_PATH)'"'
endif

ifdef FONT_PATH
CFLAGS+= -DFONT_PATH='"'$(FONT_PATH)'"'
endif

ALL=dis$(EXE) zdis$(EXE) blastem$(EXE)
ifneq ($(OS),Windows)
ALL+= termhelper
endif
DISOBJS:=dis.o disasm.o backend.o 68kinst.o tern.o vos_program_module.o util.o
MTESTOBJS:=trans.o serialize.o $(M68KOBJS) $(TRANSOBJS) util.o
ZTESTOBJS:=ztestrun.o serialize.o $(Z80OBJS) $(TRANSOBJS) util.o
CPMOBJS:=blastcpm.o util.o serialize.o $(Z80OBJS) $(TRANSOBJS)

LIBCFLAGS=$(CFLAGS) -fpic -DIS_LIB -DDISABLE_ZLIB

-include $(MAINOBJS:%.o=$(OBJDIR)/%.d)
-include $(LIBOBJS:%.o=$(LIBOBJDIR)/%.d)
-include $(DISOBJS:.o=$(OBJDIR)/%.d)
-include $(OBJDIR)/trans.d
-include $(OBJDIR)/ztestrun.d
-include $(OBJDIR)/blastcpm.d

all : $(ALL)

$(OBJDIR) :
	mkdir -p $(OBJDIR)/nuklear_ui
	mkdir -p $(OBJDIR)/zlib

$(LIBOBJDIR) :
	mkdir -p $(LIBOBJDIR)/zlib

libblastem.$(SO) : $(LIBOBJS:%.o=$(LIBOBJDIR)/%.o)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

blastem$(EXE) : $(MAINOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(LDFLAGS) $(PROFFLAGS)
	$(FIXUP) ./$@

termhelper : $(OBJDIR)/termhelper.o
	$(CC) -o $@ $^ $(LDFLAGS)

dis$(EXE) : $(DISOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(OPT)

jagdis : $(OBJDIR)/jagdis.o $(OBJDIR)/jagcpu.o $(OBJDIR)/tern.o
	$(CC) -o $@ $^ $(OPT)

zdis$(EXE) : $(OBJDIR)/zdis.o $(OBJDIR)/z80inst.o
	$(CC) -o $@ $^ $(OPT)

trans : $(MTESTOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(OPT)

ztestrun : $(ZTESTOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(OPT)

ztestgen : $(OBJDIR)/ztestgen.o $(OBJDIR)/z80inst.o
	$(CC) -o $@ $^ $(OPT)

blastcpm : $(CPMOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(OPT) $(PROFFLAGS)

vos_prog_info : $(OBJDIR)/vos_prog_info.o $(OBJDIR)/vos_program_module.o
	$(CC) -o $@ $^ $(OPT)

.PRECIOUS: %.c
%.c %.h : %.cpu cpu_dsl.py
	./cpu_dsl.py -d $(shell echo $@ | sed -E -e "s/^z80.*$$/$(Z80_DISPATCH)/" -e '/^goto/! s/^.*$$/call/') $< > $(shell echo $@ | sed -E 's/\.[ch]$$/./')c

%.db.c : %.db
	sed $< -e 's/"/\\"/g' -e 's/^\(.*\)$$/"\1\\n"/' -e'1s/^\(.*\)$$/const char $(shell echo $< | tr '.' '_')_data[] = \1/' -e '$$s/^\(.*\)$$/\1;/' > $@

$(OBJDIR)/%.o : %.S | $(OBJDIR)
	$(CC) -c -MMD -o $@ $<

$(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

$(OBJDIR)/%.o : %.m | $(OBJDIR)
	$(CC) $(CFLAGS) -c -MMD -o $@ $<

$(LIBOBJDIR)/%.o : %.S | $(LIBOBJDIR)
	$(CC) -c -MMD -o $@ $<

$(LIBOBJDIR)/%.o : %.c | $(LIBOBJDIR)
	$(CC) $(LIBCFLAGS) -c -MMD -o $@ $<

$(LIBOBJDIR)/%.o : %.m | $(LIBOBJDIR)
	$(CC) $(LIBCFLAGS) -c -MMD -o $@ $<

%.png : %.xcf
	convert -background none -flatten $< $@

%.tiles : %.spec
	./img2tiles.py -s $< $@

%.bin : %.s68
	vasmm68k_mot -Fbin -m68000 -no-opt -spaces -o $@ -L $@.list $<

%.md : %.s68
	vasmm68k_mot -Fbin -m68000 -no-opt -spaces -o $@ -L $@.list $<

%.bin : %.sz8
	vasmz80_mot -Fbin -spaces -o $@ $<
res.o : blastem.rc
	$(WINDRES) blastem.rc res.o

arrow.tiles : arrow.png
cursor.tiles : cursor.png
font_interlace_variable.tiles : font_interlace_variable.png
button.tiles : button.png
font.tiles : font.png

menu.bin : font_interlace_variable.tiles arrow.tiles cursor.tiles button.tiles font.tiles
tmss.md : font.tiles

clean :
	rm -rf $(ALL) trans ztestrun ztestgen *.o nuklear_ui/*.o zlib/*.o $(OBJDIR)
