project('blastem-highscore', 'c',
  version : '0.1',
)

cc = meson.get_compiler('c')

blastem_source = [
  'blastem-highscore.c',

  '../68kinst.c',
  '../arena.c',
  '../backend.c',
  '../cdd_fader.c',
  '../cdd_mcu.c',
  '../cd_graphics.c',
  '../cdimage.c',
  '../coleco.c',
  '../config.c',
  '../disasm.c',
  '../event_log.c',
  '../flac.c',
  '../gen.c',
  '../genesis.c',
  '../gen_player.c',
  '../gst.c',
  '../hash.c',
  '../i2c.c',
  '../i8255.c',
  '../io.c',
  '../jcart.c',
  '../lc8951.c',
  '../mediaplayer.c',
  '../megawifi.c',
  '../mem.c',
  '../multi_game.c',
  '../net.c',
  '../nor.c',
  '../paths.c',
  '../pico_pcm.c',
  '../psg.c',
  '../realtec.c',
  '../render_audio.c',
  '../rf5c164.c',
  '../romdb.c',
  '../saves.c',
  '../segacd.c',
  '../sega_mapper.c',
  '../serialize.c',
  '../sms.c',
  '../sft_mapper.c',
  '../system.c',
  '../terminal.c',
  '../tern.c',
  '../tern.c',
  '../util.c',
  '../vdp.c',
  '../vgm.c',
  '../wave.c',
  '../xband.c',
  '../ym2612.c',
  '../ymz263b.c',
  '../z80inst.c',
]

blastem_source += [
  '../backend_x86.c',
  '../gen_x86.c',
  '../m68k_core.c',
  '../m68k_core_x86.c',
  '../z80inst.c',
  '../z80_to_x86.c',
]

blastem_source += custom_target('rom.db.c',
  output: 'rom.db.c',
  input: meson.project_source_root() / '../rom.db',
  command: [ 'gen-db.sh', '@INPUT@' ],
  capture: true
)

blastem_deps = [
  dependency('highscore-1'),
  dependency('gio-2.0'),
  dependency('zlib'),
  cc.find_library('m', required: false),
]

blastem_c_args = [
  '-fvisibility=hidden',
  '-DIS_LIB',
  '-Wno-implicit-fallthrough',
  '-Wno-missing-field-initializers',
  '-Wno-parentheses',
  '-Wno-pedantic',
  '-Wno-pointer-sign',
  '-Wno-sign-compare',
  '-Wno-switch',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-function',
  '-Wno-unused-label',
  '-Wno-unused-parameter',
  '-Wno-unused-value',
  '-Wno-unused-variable',
]

cpu = target_machine.cpu()

if cpu == 'x86_64'
  blastem_c_args += '-DX86_64'
elif cpu == 'i686'
  blastem_c_args += '-DX86_32'
endif

blastem_include_dirs = [
  include_directories('.'),
  include_directories('..'),
]

cores_dir = get_option('libdir') / 'highscore' / 'cores'

shared_library('blastem-highscore',
  blastem_source,
  dependencies : blastem_deps,
  c_args : blastem_c_args,
  include_directories : blastem_include_dirs,
  name_prefix: '',
  install_dir : cores_dir,
  install : true
)

install_data('blastem.highscore',
  install_dir : cores_dir,
)
