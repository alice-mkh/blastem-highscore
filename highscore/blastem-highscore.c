#include "blastem-highscore.h"

#include "render_audio.h"
#include "system.h"
#include "util.h"

static BlastemCore *core;

struct _BlastemCore
{
  HsCore parent_instance;

  HsSoftwareContext *context;

  gboolean started;
  system_type stype;

  guint32 prev_input_state[2];
  guint32 input_state[2];
};

#include "libblastem-highscore.c"

G_DEFINE_FINAL_TYPE (BlastemCore, blastem_core, HS_TYPE_CORE)

static gboolean
blastem_core_load_rom (HsCore      *core,
                       const char **rom_paths,
                       int          n_rom_paths,
                       const char  *save_path,
                       GError     **error)
{
  BlastemCore *self = BLASTEM_CORE (core);
  char *data;
  gsize length;

  g_assert (n_rom_paths == 0);

  if (!g_file_get_contents (rom_paths[0], &data, &length, error))
    return FALSE;

  disable_stdout_messages ();

  media.dir = path_dirname (rom_path);
  media.name = basename_no_extension (rom_path);
  media.extension = path_extension (rom_path);

  media.buffer = malloc (nearest_pow2 (length));
  memcpy (media.buffer, data, length);
  media.size = length;
  self->stype = detect_system_type (&media);
  current_system = alloc_config_system (self->stype, &media, 0, 0);

  g_assert (current_system);

  g_set_str (&current_system->save_dir, save_path);
  g_set_str (&save_filename, save_path);

  render_audio_initialized (RENDER_AUDIO_S16,
                            hs_core_get_sample_rate (core),
                            2, 4, sizeof (int16_t));

  current_system->set_speed_percent (current_system, 100);

  update_overscan ();

  self->context = hs_core_create_software_context (core,
                                                   LINEBUF_SIZE,
                                                   294 * 2, /* Double buffering */
                                                   HS_PIXEL_FORMAT_XRGB8888_REV);

  g_free (data);

  if (current_system->persist_save)
    current_system->load_save (current_system);

  return TRUE;
}

static void
blastem_core_reset (HsCore *core)
{
  current_system->soft_reset (current_system);
}

static void
blastem_core_poll_input (HsCore *core, HsInputState *input_state)
{
  BlastemCore *self = BLASTEM_CORE (core);

  self->input_state[0] = input_state->sega_genesis.pad_buttons[0];
  self->input_state[1] = input_state->sega_genesis.pad_buttons[1];
}

static void
blastem_core_run_frame (HsCore *core)
{
  BlastemCore *self = BLASTEM_CORE (core);

  if (self->started) {
    current_system->resume_context (current_system);
  } else {
    current_system->start_context (current_system, NULL);
    self->started = TRUE;
  }
}

static void
blastem_core_stop (HsCore *core)
{
  BlastemCore *self = BLASTEM_CORE (core);

  g_clear_object (&self->context);

  free (media.dir);
  free (media.name);
  free (media.extension);
  media.dir = media.name = media.extension = NULL;
  //buffer is freed by the context
  media.buffer = NULL;

  current_system->free_context (current_system);
  current_system = NULL;

  self->started = FALSE;

  g_clear_pointer (&save_filename, g_free);
}

static gboolean
blastem_core_reload_save (HsCore      *core,
                          const char  *save_path,
                          GError     **error)
{
  g_set_str (&current_system->save_dir, save_path);
  g_set_str (&save_filename, save_path);

  if (current_system->load_save)
    current_system->load_save (current_system);

  return TRUE;
}

static gboolean
blastem_core_sync_save (HsCore  *core,
                        GError **error)
{
  if (current_system->persist_save)
    current_system->persist_save (current_system);

  return TRUE;
}

static void
blastem_core_save_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  size_t size;
  g_autofree guint8 *data = current_system->serialize (current_system, &size);
  GError *error = NULL;

  if (!g_file_replace_contents (file, data, size,
                                NULL, FALSE, G_FILE_CREATE_NONE,
                                NULL, NULL, &error)) {
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static void
blastem_core_load_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  GError *error = NULL;
  g_autofree char *data = NULL;
  size_t size;

  if (!g_file_load_contents (file, NULL, &data, &size, NULL, &error)) {
    callback (core, &error);
    return;
  }

  current_system->deserialize (current_system, (guint8 *) (data), size);

  callback (core, NULL);
}

static double
blastem_core_get_frame_rate (HsCore *core)
{
  double master_clock = video_standard == VID_NTSC ? 53693175 : 53203395;
  double lines = video_standard == VID_NTSC ? 262 : 313;

  return master_clock / (3420.0 * lines);
}

static double
blastem_core_get_aspect_ratio (HsCore *core)
{
  guint game_height = video_standard == VID_NTSC ? 243 : 294;
  guint width = LINEBUF_SIZE - overscan_left - overscan_right;
  guint height = game_height - overscan_top - overscan_bot;

  return (double) width / height;
}

static double
blastem_core_get_sample_rate (HsCore *core)
{
  double master_clock = video_standard == VID_NTSC ? 53693175 : 53203395;

  return master_clock / (7 * 6 * 24); //sample rate of YM2612
}

static void
blastem_core_finalize (GObject *object)
{
  G_OBJECT_CLASS (blastem_core_parent_class)->finalize (object);

  core = NULL;
}

static void
blastem_core_class_init (BlastemCoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  object_class->finalize = blastem_core_finalize;

  core_class->load_rom = blastem_core_load_rom;
  core_class->reset = blastem_core_reset;
  core_class->poll_input = blastem_core_poll_input;
  core_class->run_frame = blastem_core_run_frame;
  core_class->stop = blastem_core_stop;

  core_class->reload_save = blastem_core_reload_save;
  core_class->sync_save = blastem_core_sync_save;

  core_class->save_state = blastem_core_save_state;
  core_class->load_state = blastem_core_load_state;

  core_class->get_frame_rate = blastem_core_get_frame_rate;
  core_class->get_aspect_ratio = blastem_core_get_aspect_ratio;

  core_class->get_sample_rate = blastem_core_get_sample_rate;
}

static void
blastem_core_init (BlastemCore *self)
{
  g_assert (!core);

  core = self;
}

GType
hs_get_core_type (void)
{
  return BLASTEM_TYPE_CORE;
}
