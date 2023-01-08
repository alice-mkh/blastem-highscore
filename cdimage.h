#ifndef CUE_H_
#define CUE_H_

uint8_t parse_cue(system_media *media);
uint8_t parse_toc(system_media *media);
uint32_t make_iso_media(system_media *media, const char *filename);
void cdimage_serialize(system_media *media, serialize_buffer *buf);
void cdimage_deserialize(deserialize_buffer *buf, void *vmedia);

#endif //CUE_H_
