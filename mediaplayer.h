#ifndef MEDIAPLAYER_H_
#define MEDIAPLAYER_H_

#include <stdint.h>
#include "system.h"
#include "vgm.h"

typedef struct chip_info chip_info;
typedef void (*chip_run_fun)(void *context, uint32_t cycle);
typedef void (*chip_adjust_fun)(chip_info *chip);
struct chip_info {
	void            *context;
	chip_run_fun    run;
	chip_adjust_fun adjust;
	data_block      *blocks;
	uint32_t        clock;
	uint32_t        samples;
	uint8_t         cmd;
	uint8_t         data_type;
};

typedef struct {
	system_header       header;
	system_media        *media;
	vgm_header          *vgm;
	vgm_extended_header *vgm_ext;
	data_block          *ym_seek_block;
	chip_info           *chips;
	uint32_t            num_chips;
	uint32_t            current_offset;
	uint32_t            playback_time;
	uint32_t            wait_samples;
	uint32_t            ym_seek_offset;
	uint32_t            ym_block_offset;
	uint8_t             state;
	uint8_t             media_type;
	uint8_t             should_return;
} media_player;

media_player *alloc_media_player(system_media *media, uint32_t opts);


#endif //MEDIAPLAYER_H_
