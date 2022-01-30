#include "cdd_mcu.h"
#include "backend.h"

#define SCD_MCLKS 50000000
#define CD_BLOCK_CLKS 16934400
#define CDD_MCU_DIVIDER 8
#define SECTOR_CLOCKS (CD_BLOCK_CLKS/75)
#define NIBBLE_CLOCKS (CDD_MCU_DIVIDER * 77)
#define BYTE_CLOCKS (SECTOR_CLOCKS/2352) // 96

//lead in start max diameter 46 mm
//program area start max diameter 50 mm
//difference 4 mm = 4000 um
//radius difference 2 mm = 2000 um
//track pitch 1.6 um
//1250 physical tracks in between
//linear speed 1.2 m/s - 1.4 m/s
// 1.3 m = 1300 mm
// circumference at 46 mm ~ 144.51 mm
// circumference at 50 mm ~ 157.08 mm
// avg is 150.795
// 75 sectors per second
// 17.3333 mm "typical" length of a sector
// ~8.7 sectors per track in lead-in area
#define LEADIN_SECTORS 10875

static uint32_t cd_block_to_mclks(uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)SCD_MCLKS) / ((uint64_t)CD_BLOCK_CLKS);
}

static uint32_t mclks_to_cd_block(uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)CD_BLOCK_CLKS) / ((uint64_t)SCD_MCLKS);
}

void cdd_mcu_init(cdd_mcu *context, system_media *media)
{
	context->next_int_cycle = CYCLE_NEVER;
	context->last_subcode_cycle = CYCLE_NEVER;
	context->last_nibble_cycle = CYCLE_NEVER;
	context->requested_format = SF_NOTREADY;
	context->media = media;
	context->current_status_nibble = -1;
	context->current_cmd_nibble = -1;
}

enum {
	GAO_CDD_CTRL,
	GAO_CDD_STATUS,
	GAO_CDD_CMD = GAO_CDD_STATUS+5
};

static uint8_t checksum(uint8_t *vbuffer)
{
	uint8_t *buffer = vbuffer;
	uint8_t sum = 0;
	for (int i = 0; i < 9; i++)
	{
		sum += buffer[i];
	}
	return (~sum) & 0xF;
}
#define COARSE_SEEK 2200 //made up numbers
#define FINE_SEEK 10
static void handle_seek(cdd_mcu *context)
{
	//TODO: more realistic seeking behavior
	if (context->seeking) {
		if (context->seek_pba == context->head_pba) {
			context->seeking = 0;
		} else if (context->seek_pba > context->head_pba) {
			if (context->seek_pba - context->head_pba >= COARSE_SEEK) {
				context->head_pba += COARSE_SEEK;
			} else if (context->seek_pba - context->head_pba >= FINE_SEEK) {
				context->head_pba += FINE_SEEK;
			} else {
				context->head_pba++;
			}
		} else {
			if (context->head_pba - context->seek_pba >= COARSE_SEEK) {
				context->head_pba -= COARSE_SEEK;
			} else if (context->head_pba >= FINE_SEEK) {
				context->head_pba -= FINE_SEEK;
			} else {
				context->head_pba = 0;
			}
		}
	}
}

static void lba_to_status(cdd_mcu *context, uint32_t lba)
{
	uint32_t seconds = lba / 75;
	uint32_t frames = lba % 75;
	uint32_t minutes = seconds / 60;
	seconds = seconds % 60;
	context->status_buffer.b.time.min_high = minutes / 10;
	context->status_buffer.b.time.min_low = minutes % 10;
	context->status_buffer.b.time.sec_high = seconds / 10;
	context->status_buffer.b.time.sec_low = seconds % 10;
	context->status_buffer.b.time.frame_high = frames / 10;
	context->status_buffer.b.time.frame_low = frames % 10;
}

static void update_status(cdd_mcu *context)
{
	switch (context->status)
	{
	case DS_PLAY:
		handle_seek(context);
		if (!context->seeking) {
			context->head_pba++;
			uint32_t lba = context->head_pba - LEADIN_SECTORS;
			for (uint32_t i = 0; i < context->media->num_tracks; i++)
			{
				if (lba < context->media->tracks[i].fake_pregap) {
					context->in_fake_pregap = 1;
					break;
				}
				lba -= context->media->tracks[i].fake_pregap;
				if (lba < context->media->tracks[i].start_lba) {
					context->in_fake_pregap = 1;
					break;
				}
				if (lba < context->media->tracks[i].end_lba) {
					fseek(context->media->f, lba * 2352, SEEK_SET);
					context->in_fake_pregap = 0;
					break;
				}
			}
		}
		break;
	case DS_PAUSE:
		handle_seek(context);
		break;
	case DS_TOC_READ:
		handle_seek(context);
		if (!context->seeking) {
			context->head_pba++;
			if (context->media && context->media->type == MEDIA_CDROM && context->media->num_tracks) {
				if (context->head_pba > 3*context->media->num_tracks + 1) {
					context->toc_valid = 1;
					context->seeking = 1;
					context->seek_pba = LEADIN_SECTORS + context->media->tracks[0].start_lba;
					context->status = DS_PAUSE;
				}

			} else {
				context->status = DS_NO_DISC;
			}
		}
		break;

	}
	if (context->first_cmd_received) {
		switch (context->requested_format)
		{
		case SF_ABSOLUTE:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				lba_to_status(context, context->head_pba - LEADIN_SECTORS);
				context->status_buffer.format = SF_ABSOLUTE;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_RELATIVE:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				uint32_t lba =context->head_pba - LEADIN_SECTORS;
				for (uint32_t i = 0; i < context->media->num_tracks; i++)
				{
					if (lba < context->media->tracks[i].end_lba) {
						if (context->media->tracks[i].fake_pregap) {
							if (lba > context->media->tracks[i].fake_pregap) {
								lba -= context->media->tracks[i].fake_pregap;
							} else {
								//relative time counts down to 0 in pregap
								lba = context->media->tracks[i].fake_pregap - lba;
								break;
							}
						}
						if (lba < context->media->tracks[i].start_lba) {
							//relative time counts down to 0 in pregap
							lba = context->media->tracks[i].start_lba - lba;
						} else {
							lba -= context->media->tracks[i].start_lba;
						}
						break;
					} else if (context->media->tracks[i].fake_pregap) {
						lba -= context->media->tracks[i].fake_pregap;
					}
				}
				lba_to_status(context, lba);
				context->status_buffer.format = SF_RELATIVE;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TRACK:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				uint32_t lba =context->head_pba - LEADIN_SECTORS;
				uint32_t i;
				for (i = 0; i < context->media->num_tracks; i++)
				{
					if (lba < context->media->tracks[i].end_lba) {
						if (context->media->tracks[i].fake_pregap) {
							if (lba > context->media->tracks[i].fake_pregap) {
								lba -= context->media->tracks[i].fake_pregap;
							} else {
								//relative time counts down to 0 in pregap
								lba = context->media->tracks[i].fake_pregap - lba;
								break;
							}
						}
						if (lba < context->media->tracks[i].start_lba) {
							//relative time counts down to 0 in pregap
							lba = context->media->tracks[i].start_lba - lba;
						} else {
							lba -= context->media->tracks[i].start_lba;
						}
						break;
					} else if (context->media->tracks[i].fake_pregap) {
						lba -= context->media->tracks[i].fake_pregap;
					}
				}
				context->status_buffer.b.track.track_high = (i + 1) / 10;
				context->status_buffer.b.track.track_low = (i + 1) % 10;
				if (context->media->tracks[i].type == TRACK_DATA) {
					context->status_buffer.b.track.control = 4;
				} else {
					//TODO: pre-emphasis flag
					//TODO: copy permitted flag
					context->status_buffer.b.track.control = 0;
				}
				context->status_buffer.b.track.adr = 1;
				context->status_buffer.format = SF_TRACK;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCO:
			if (context->toc_valid) {
				uint32_t total_fake_pregap = 0;
				for (uint32_t i = 0; i < context->media->num_tracks; i++)
				{
					total_fake_pregap += context->media->tracks[i].fake_pregap;
				}
				lba_to_status(context, context->media->tracks[context->media->num_tracks - 1].end_lba + total_fake_pregap);
				context->status_buffer.format = SF_TOCO;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCT:
			if (context->toc_valid) {
				context->status_buffer.b.toct.first_track_high = 0;
				context->status_buffer.b.toct.first_track_low = 1;
				context->status_buffer.b.toct.last_track_high = (context->media->num_tracks + 1) / 10;
				context->status_buffer.b.toct.last_track_low = (context->media->num_tracks + 1) % 10;
				context->status_buffer.b.toct.version = 0;
				context->status_buffer.format = SF_TOCT;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCN:
			if (context->toc_valid) {
				uint32_t lba = context->media->tracks[context->requested_track - 1].start_lba;
				for (uint32_t i = 0; i < context->requested_track; i++) {
					lba += context->media->tracks[i].fake_pregap;
				}
				lba_to_status(context, lba);
				if (context->media->tracks[context->requested_track - 1].type == TRACK_DATA) {
					context->status_buffer.b.tocn.frame_low |= 0x80;
				}
				context->status_buffer.b.tocn.track_low = context->requested_track % 10;
				context->status_buffer.format = SF_TOCN;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_NOTREADY:
			memset(&context->status_buffer, 0, sizeof(context->status_buffer) - 1);
			context->status_buffer.format = SF_NOTREADY;
			break;
		}
		if (context->error_status == DS_STOP) {
			if (context->requested_format >= SF_TOCO && context->requested_format <= SF_TOCN) {
				context->status_buffer.status = DS_TOC_READ;
			} else if (context->seeking) {
				context->status_buffer.status = DS_SEEK;
			} else {
				context->status_buffer.status = context->status;
			}
		} else {
			context->status_buffer.status = context->error_status;
			context->status_buffer.format = SF_NOTREADY;
			context->error_status = DS_STOP;
		}
		if (context->requested_format != SF_TOCN) {
			context->status_buffer.b.time.flags = 1; //TODO: populate these
		}
	} else {
		// Did not receive our first command so just send zeroes
		memset(&context->status_buffer, 0, sizeof(context->status_buffer) - 1);
	}
	context->status_buffer.checksum = checksum((uint8_t *)&context->status_buffer);
	if (context->status_buffer.format != SF_NOTREADY) {
		printf("CDD Status %X%X.%X%X%X%X%X%X.%X%X\n",
			context->status_buffer.status, context->status_buffer.format,
			context->status_buffer.b.time.min_high, context->status_buffer.b.time.min_low,
			context->status_buffer.b.time.sec_high, context->status_buffer.b.time.sec_low,
			context->status_buffer.b.time.frame_high, context->status_buffer.b.time.frame_low,
			context->status_buffer.b.time.flags, context->status_buffer.checksum
		);
	}
}

static void run_command(cdd_mcu *context)
{
	uint8_t check = checksum((uint8_t*)&context->cmd_buffer);
	printf("cmd %X, checksum: %X, calc: %X\n", context->cmd_buffer.cmd_type, context->cmd_buffer.checksum, check);
	if (check != context->cmd_buffer.checksum) {
		context->error_status = DS_SUM_ERROR;
		return;
	}
	if (context->cmd_buffer.must_be_zero) {
		context->error_status = DS_CMD_ERROR;
		return;
	}
	context->first_cmd_received = 1;
	switch (context->cmd_buffer.cmd_type)
	{
	case CMD_NOP:
		break;
	case CMD_STOP:
		puts("CDD CMD: STOP");
		context->status = DS_STOP;
		context->requested_format = SF_ABSOLUTE;
		break;
	case CMD_READ:
	case CMD_SEEK: {
		if (context->status == DS_DOOR_OPEN || context->status == DS_TRAY_MOVING || context->status == DS_DISC_LEADOUT || context->status == DS_DISC_LEADIN) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		uint32_t lba = context->cmd_buffer.b.time.min_high * 10 + context->cmd_buffer.b.time.min_low;
		lba *= 60;
		lba += context->cmd_buffer.b.time.sec_high * 10 + context->cmd_buffer.b.time.sec_low;
		lba *= 75;
		lba += context->cmd_buffer.b.time.frame_high * 10 + context->cmd_buffer.b.time.frame_low;
		printf("CDD CMD: %s cmd for lba %d, MM:SS:FF %u%u:%u%u:%u%u\n",
			context->cmd_buffer.cmd_type == CMD_READ ? "READ" : "SEEK", lba,
			context->cmd_buffer.b.time.min_high, context->cmd_buffer.b.time.min_low,
			context->cmd_buffer.b.time.sec_high, context->cmd_buffer.b.time.sec_low,
			context->cmd_buffer.b.time.frame_high, context->cmd_buffer.b.time.frame_low
		);
		if (lba >= context->media->tracks[0].fake_pregap + context->media->tracks[context->media->num_tracks - 1].end_lba) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		context->seek_pba = lba + LEADIN_SECTORS - 4;
		context->seeking = 1;
		context->status = context->cmd_buffer.cmd_type == CMD_READ ? DS_PLAY : DS_PAUSE;
		break;
	}
	case CMD_REPORT_REQUEST:
		switch (context->cmd_buffer.b.format.status_type)
		{
		case SF_ABSOLUTE:
		case SF_RELATIVE:
		case SF_TRACK:
			context->requested_format = context->cmd_buffer.b.format.status_type;
			break;
		case SF_TOCO:
			if (context->toc_valid) {
				context->requested_format = SF_TOCO;
			} else {
				context->error_status = DS_CMD_ERROR;
				context->requested_format = SF_ABSOLUTE;
			}
			break;
		case SF_TOCT:
			if (context->toc_valid) {
				if (context->status == DS_STOP) {
					context->status = DS_TOC_READ;
					context->seeking = 1;
					context->seek_pba = 0;
				}
			} else {
				context->status = DS_TOC_READ;
				context->seeking = 1;
				context->seek_pba = 0;
			}
			context->requested_format = SF_TOCT;
			break;
		case SF_TOCN:
			context->requested_track = context->cmd_buffer.b.format.track_high * 10;
			context->requested_track += context->cmd_buffer.b.format.track_low;
			if (!context->media || context->requested_track > context->media->num_tracks) {
				context->requested_format = SF_ABSOLUTE;
				context->error_status = DS_CMD_ERROR;
			}
			context->status = DS_TOC_READ;
			context->seeking = 1;
			context->seek_pba = 0;
			context->requested_format = SF_TOCN;
			break;
		}
		printf("CDD CMD: REPORT REQUEST(%d), format set to %d\n", context->cmd_buffer.b.format.status_type, context->requested_format);
		break;
	case CMD_PAUSE:
		if (context->status == DS_DOOR_OPEN || context->status == DS_TRAY_MOVING || context->status == DS_DISC_LEADOUT || context->status == DS_DISC_LEADIN) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->status == DS_STOP) {
			context->seeking = 1;
			context->seek_pba = LEADIN_SECTORS + context->media->tracks[0].fake_pregap + context->media->tracks[0].start_lba;
			printf("CDD CMD: PAUSE, seeking to %u\n", context->seek_pba);
		} else {
			puts("CDD CMD: PAUSE");
		}
		context->status = DS_PAUSE;
		break;
	default:
		printf("CDD CMD: Unimplemented(%d)\n", context->cmd_buffer.cmd_type);
	}
}

#define BIT_HOCK 0x4
#define BIT_DRS  0x2
#define BIT_DTS  0x1

void cdd_mcu_run(cdd_mcu *context, uint32_t cycle, uint16_t *gate_array, lc8951* cdc)
{
	uint32_t cd_cycle = mclks_to_cd_block(cycle);
	if (!(gate_array[GAO_CDD_CTRL] & BIT_HOCK)) {
		//it's a little unclear if this gates the actual cd block clock or just handshaking
		//assum it's actually the clock for now
		context->cycle = cd_cycle;
		return;
	}
	uint32_t next_subcode = context->last_subcode_cycle + SECTOR_CLOCKS;
	uint32_t next_nibble = context->current_status_nibble >= 0 ? context->last_nibble_cycle + NIBBLE_CLOCKS : CYCLE_NEVER;
	uint32_t next_cmd_nibble = context->current_cmd_nibble >= 0 ? context->last_nibble_cycle + NIBBLE_CLOCKS : CYCLE_NEVER;
	uint32_t next_byte = context->current_sector_byte >= 0 ? context->last_byte_cycle + BYTE_CLOCKS : CYCLE_NEVER;
	for (; context->cycle < cd_cycle; context->cycle += CDD_MCU_DIVIDER)
	{
		if (context->cycle >= next_subcode) {
			context->last_subcode_cycle = context->cycle;
			next_subcode = context->cycle + SECTOR_CLOCKS;
			update_status(context);
			next_nibble = context->cycle;
			context->current_status_nibble = 0;
			gate_array[GAO_CDD_STATUS] |= BIT_DRS;
			if (context->status == DS_PLAY && !context->seeking) {
				next_byte = context->cycle;
				context->current_sector_byte = 0;
			}
		}
		if (context->cycle >= next_nibble) {
			if (context->current_status_nibble == sizeof(cdd_status)) {
				context->current_status_nibble = -1;
				gate_array[GAO_CDD_STATUS] &= ~BIT_DRS;
				if (context->cmd_recv_pending) {
					context->cmd_recv_pending = 0;
					context->current_cmd_nibble = 0;
					gate_array[GAO_CDD_STATUS] |= BIT_DTS;
					next_nibble = context->cycle + NIBBLE_CLOCKS;
				} else {
					context->cmd_recv_wait = 1;
					next_nibble = CYCLE_NEVER;
				}
			} else {
				uint8_t value = ((uint8_t *)&context->status_buffer)[context->current_status_nibble];
				int ga_index = GAO_CDD_STATUS + (context->current_status_nibble >> 1);
				if (context->current_status_nibble & 1) {
					gate_array[ga_index] = value | (gate_array[ga_index]  & 0xFF00);
				} else {
					gate_array[ga_index] = (value << 8) | (gate_array[ga_index]  & 0x00FF);
				}
				if (context->current_status_nibble == 7) {
					context->int_pending = 1;
					context->next_int_cycle = cd_block_to_mclks(cycle + SECTOR_CLOCKS);
				}
				context->current_status_nibble++;
				context->last_nibble_cycle = context->cycle;
				next_nibble = context->cycle + NIBBLE_CLOCKS;
			}
		} else if (context->cycle >= next_cmd_nibble) {
			if (context->current_cmd_nibble == sizeof(cdd_cmd)) {
				next_cmd_nibble = CYCLE_NEVER;
				context->current_cmd_nibble = -1;
				gate_array[GAO_CDD_STATUS] &= ~BIT_DTS;
				run_command(context);
			} else {
				int ga_index = GAO_CDD_CMD + (context->current_cmd_nibble >> 1);
				uint8_t value = (context->current_cmd_nibble & 1) ? gate_array[ga_index] : gate_array[ga_index] >> 8;
				((uint8_t *)&context->cmd_buffer)[context->current_cmd_nibble] = value;
				context->current_cmd_nibble++;
				context->last_nibble_cycle = context->cycle;
				next_cmd_nibble = context->cycle + NIBBLE_CLOCKS;
			}
		}
		if (context->cycle >= next_byte) {
			uint8_t byte;
			if (context->in_fake_pregap) {
				if (!context->current_sector_byte || (context->current_sector_byte >= 16)) {
					byte = 0;
					//TODO: error detection and correction bytes
				} else if (context->current_sector_byte < 12) {
					byte = 0xFF;
				} else if (context->current_sector_byte == 12) {
					uint32_t minute = ((context->head_pba - LEADIN_SECTORS) / 75) / 60;
					byte = (minute % 10) | ((minute / 10 ) << 4);
				} else if (context->current_sector_byte == 13) {
					uint32_t seconds = ((context->head_pba - LEADIN_SECTORS) / 75) % 60;
					byte = (seconds % 10) | ((seconds / 10 ) << 4);
				} else if (context->current_sector_byte == 14) {
					uint32_t frames = (context->head_pba - LEADIN_SECTORS) % 75;
					byte = (frames % 10) | ((frames / 10 ) << 4);
				} else {
					byte = 1;
				}
			} else {
				byte = fgetc(context->media->f);
			}
			lc8951_write_byte(cdc, cd_block_to_mclks(context->cycle), context->current_sector_byte++, byte);
			context->last_byte_cycle = context->cycle;
			next_byte = context->cycle + BYTE_CLOCKS;
			if (context->current_sector_byte == 2352) {
				context->current_sector_byte = -1;

			}
		}
	}
}

void cdd_mcu_start_cmd_recv(cdd_mcu *context, uint16_t *gate_array)
{
	if (context->cmd_recv_wait) {
		context->current_cmd_nibble = 0;
		gate_array[GAO_CDD_STATUS] |= BIT_DTS;
		context->last_nibble_cycle = context->cycle;
		context->cmd_recv_wait = 0;
	} else {
		context->cmd_recv_pending = 1;
	}
}

void cdd_hock_enabled(cdd_mcu *context)
{
	context->last_subcode_cycle = context->cycle;
	context->next_int_cycle = cd_block_to_mclks(context->cycle + SECTOR_CLOCKS + 7 * NIBBLE_CLOCKS);
}

void cdd_hock_disabled(cdd_mcu *context)
{
	context->last_subcode_cycle = CYCLE_NEVER;
	context->next_int_cycle = CYCLE_NEVER;
	context->last_nibble_cycle = CYCLE_NEVER;
	context->current_status_nibble = -1;
	context->current_cmd_nibble = -1;
}

void cdd_mcu_adjust_cycle(cdd_mcu *context, uint32_t deduction)
{
	uint32_t cd_deduction = mclks_to_cd_block(deduction);
	if (context->next_int_cycle != CYCLE_NEVER) {
		context->next_int_cycle -= deduction;
	}
	if (context->last_subcode_cycle != CYCLE_NEVER) {
		context->last_subcode_cycle -= cd_deduction;
	}
	if (context->last_nibble_cycle != CYCLE_NEVER) {
		context->last_nibble_cycle -= cd_deduction;
	}
	if (context->last_byte_cycle != CYCLE_NEVER) {
		context->last_byte_cycle -= cd_deduction;
	}
}
