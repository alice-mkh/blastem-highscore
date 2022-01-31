#include <ctype.h>
#include <string.h>

#include "system.h"
#include "util.h"

static char* cmd_start(char *cur)
{
	while (*cur && isblank(*cur))
	{
		cur++;
	}
	return cur;
}

static char* next_line(char *cur)
{
	while (*cur && *cur != '\n')
	{
		cur++;
	}
	if (*cur) {
		return cur + 1;
	}
	return NULL;
}

static uint32_t timecode_to_lba(char *timecode)
{
	char *end;
	int seconds = 0, frames = 0;
	int minutes = strtol(timecode, &end, 10);
	if (end) {
		timecode = end + 1;
		seconds = strtol(timecode, &end, 10);
		if (end) {
			timecode = end + 1;
			frames = strtol(timecode, NULL, 10);
		}
	}
	seconds += minutes * 60;
	return seconds * 75 + frames;

}

uint8_t parse_cue(system_media *media)
{
	char *line = media->buffer;
	media->num_tracks = 0;
	do {
		char *cmd = cmd_start(line);
		if (cmd) {
			if (startswith(cmd, "TRACK ")) {
				media->num_tracks++;
			}
			line = next_line(cmd);
		} else {
			line = NULL;
		}
	} while (line);
	track_info *tracks = calloc(sizeof(track_info), media->num_tracks);
	media->tracks = tracks;
	line = media->buffer;
	int track = -1;
	do {
		char *cmd = cmd_start(line);
		if (cmd) {
			if (startswith(cmd, "TRACK ")) {
				track++;
				cmd += 6;
				char *end;
				int file_track = strtol(cmd, &end, 10);
				if (file_track != (track + 1)) {
					warning("Expected track %d, but found track %d in CUE sheet\n", track + 1, file_track);
				}
				cmd = cmd_start(end);
				if (cmd) {
					tracks[track].type = startswith(cmd, "AUDIO") ? TRACK_AUDIO : TRACK_DATA;
				}
			} else if (startswith(cmd, "FILE ")) {
				if (media->f) {
					warning("CUE sheets with multiple FILE commands are not supported\n");
				} else {
					cmd += 5;
					cmd = strchr(cmd, '"');
					if (cmd) {
						cmd++;
						char *end = strchr(cmd, '"');
						if (end) {
							char *fname;
							//TODO: zipped BIN/CUE support
							if (is_absolute_path(cmd)) {
								fname = malloc(end-cmd + 1);
								memcpy(fname, cmd, end-cmd);
								fname[end-cmd] = 0;
							} else {
								size_t dirlen = strlen(media->dir);
								fname = malloc(dirlen + 1 + (end-cmd) + 1);
								memcpy(fname, media->dir, dirlen);
								fname[dirlen] = PATH_SEP[0];
								memcpy(fname + dirlen + 1, cmd, end-cmd);
								fname[dirlen + 1 + (end-cmd)] = 0;
							}
							media->f = fopen(fname, "rb");
							if (!media->f) {
								fatal_error("Failed to open %s specified by FILE command in CUE sheet %s.%s\n", fname, media->name, media->extension);
							}
							free(fname);
						}
					}
				}
			} else if (track >= 0) {
				if (startswith(cmd, "PREGAP ")) {
					tracks[track].fake_pregap = timecode_to_lba(cmd + 7);
				} else if (startswith(cmd, "INDEX ")) {
					char *after;
					int index = strtol(cmd + 6, &after, 10);
					if (!index) {
						tracks[track].pregap_lba = timecode_to_lba(after);
					} else if (index == 1) {
						tracks[track].start_lba = timecode_to_lba(after);
					}
				}
			}
			if (cmd) {
				line = next_line(cmd);
			} else {
				line = NULL;
			}
		} else {
			line = NULL;
		}
	} while (line);
	for (uint32_t i = 0; i < (media->num_tracks - 1); i++)
	{
		uint32_t next = i + 1;
		tracks[i].end_lba = tracks[next].pregap_lba ? tracks[next].pregap_lba : tracks[next].start_lba;
	}
	if (media->f) {
		//end of last track is implicitly defined by file size
		tracks[media->num_tracks-1].end_lba = file_size(media->f) / 2352;
		//replace cue sheet with first sector
		free(media->buffer);
		media->buffer = calloc(2048, 1);
		if (tracks[0].type = TRACK_DATA) {
			// if the first track is a data track, don't trust the CUE sheet and look at the MM:SS:FF from first sector
			uint8_t msf[3];
			fseek(media->f, 12, SEEK_SET);
			if (sizeof(msf) == fread(msf, 1, sizeof(msf), media->f)) {
				tracks[0].fake_pregap = msf[2] + (msf[0] * 60 + msf[1]) * 75;
			}
		}

		fseek(media->f, 16, SEEK_SET);
		media->size = fread(media->buffer, 1, 2048, media->f);
	}
	uint8_t valid = tracks > 0 && media->f != NULL;
	media->type = valid ? MEDIA_CDROM : MEDIA_CART;
	return valid;
}
