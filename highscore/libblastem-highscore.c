#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "render.h"
#include "system.h"

static vid_std video_standard;

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
char *save_filename;
tern_node *config;
uint8_t use_native_states = 1;
system_header *current_system;

uint8_t sega_cd_region;

static uint8_t last_fb;

static uint32_t overscan_top, overscan_bot, overscan_left, overscan_right;
static uint32_t last_width, last_height;

static system_media media;
const system_media *current_media(void)
{
  return &media;
}

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
  return r << 16 | g << 8 | b;
}

uint8_t render_create_window(char *caption, uint32_t width, uint32_t height, window_close_handler close_handler)
{
  //not supported in lib build
  return 0;
}

void render_destroy_window(uint8_t which)
{
  //not supported in lib build
}

uint8_t render_get_active_framebuffer(void)
{
  return 0;
}

uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
  BlastemCore *self = BLASTEM_CORE (core);

  g_assert (which == FRAMEBUFFER_ODD || which == FRAMEBUFFER_EVEN);

  if (!self->context)
    return NULL;

  uint *fb = hs_software_context_get_framebuffer (self->context);

  *pitch = LINEBUF_SIZE * sizeof(uint32_t);
  if (which != last_fb)
    *pitch *= 2;

  if (which == FRAMEBUFFER_EVEN)
    return fb + LINEBUF_SIZE;

  return fb;
}

void render_framebuffer_updated(uint8_t which, int width)
{
  BlastemCore *self = BLASTEM_CORE (core);

  if (!self->context) {
    system_request_exit (current_system, 0);
    return;
  }

  unsigned game_height = video_standard == VID_NTSC ? 243 : 294;
  unsigned height_multiplier = 1;

  if (which != last_fb) {
    height_multiplier = 2;
    last_fb = which;
  }

  HsRectangle area = HS_RECTANGLE_INIT (overscan_left, overscan_top * height_multiplier,
                                        width - overscan_left - overscan_right,
                                        (game_height - overscan_top - overscan_bot) * height_multiplier);

  hs_software_context_set_area (self->context, &area);

  system_request_exit (current_system, 0);
}

void render_set_video_standard(vid_std std)
{
  video_standard = std;
}

uint8_t render_fullscreen(void)
{
  return 1;
}

uint32_t render_overscan_top()
{
  return overscan_top;
}

uint32_t render_overscan_bot()
{
  return overscan_bot;
}

void process_events()
{
  BlastemCore *self = BLASTEM_CORE (core);

  static const uint8_t button_map[] = {
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    BUTTON_A, BUTTON_B, BUTTON_C,
    BUTTON_X, BUTTON_Y, BUTTON_Z,
    BUTTON_START, BUTTON_MODE
  };

  // TODO: handle other input device types
  // TODO: handle more than 2 ports when appropriate

  for (int player = 0; player < 2; player++) {
    for (int button = 0; button < HS_MEGA_DRIVE_BUTTON_MODE; button++) {
      gboolean old_state = self->prev_input_state[player] & 1 << button;
      gboolean new_state = self->input_state[player] & 1 << button;

      if (old_state == new_state)
        continue;

      if (new_state)
        current_system->gamepad_down (current_system, player + 1, button_map[button]);
      else
        current_system->gamepad_up (current_system, player + 1, button_map[button]);
    }

    self->prev_input_state[player] = self->input_state[player];
  }
}

void render_errorbox(char *title, char *message)
{
}
void render_warnbox(char *title, char *message)
{
}
void render_infobox(char *title, char *message)
{
}

uint8_t render_is_audio_sync(void)
{
  return 1;
}

uint8_t render_should_release_on_exit(void)
{
  return 0;
}

void render_buffer_consumed(audio_source *src)
{
}

void *render_new_audio_opaque(void)
{
  return NULL;
}

void render_free_audio_opaque(void *opaque)
{
}

void render_lock_audio(void)
{
}

void render_unlock_audio()
{
}

uint32_t render_min_buffered(void)
{
  //not actually used in the sync to audio path
  return 4;
}

uint32_t render_audio_syncs_per_sec(void)
{
  return 0;
}

void render_audio_created(audio_source *src)
{
}

void render_do_audio_ready(audio_source *src)
{
  int16_t *tmp = src->front;
  src->front = src->back;
  src->back = tmp;
  src->front_populated = 1;
  src->buffer_pos = 0;
  if (all_sources_ready()) {
    int16_t buffer[8];
    int min_remaining_out;
    mix_and_convert((uint8_t *)buffer, sizeof(buffer), &min_remaining_out);
    hs_core_play_samples (HS_CORE (core), buffer, sizeof(buffer) / (sizeof(*buffer)));
  }
}

void render_source_paused(audio_source *src, uint8_t remaining_sources)
{
}

void render_source_resumed(audio_source *src)
{
}

void render_set_external_sync(uint8_t ext_sync_on)
{
}

void bindings_set_mouse_mode(uint8_t mode)
{
}

void bindings_release_capture(void)
{
}

void bindings_reacquire_capture(void)
{
}

extern const char rom_db_data[];
char *read_bundled_file(char *name, uint32_t *sizeret)
{
  if (!strcmp(name, "rom.db")) {
    *sizeret = strlen(rom_db_data);
    char *ret = malloc(*sizeret+1);
    memcpy(ret, rom_db_data, *sizeret + 1);
    return ret;
  }

  return NULL;
}

static void update_overscan(void)
{
  if (video_standard == VID_NTSC) {
    overscan_top = 11;
    overscan_bot = 8;
    overscan_left = 13;
    overscan_right = 14;
  } else {
    overscan_top = 30;
    overscan_bot = 24;
    overscan_left = 13;
    overscan_right = 14;
  }
}
