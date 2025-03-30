#ifndef PPM_H_
#define PPM_H_
#include "pixel.h"

void save_ppm(FILE *f, pixel_t *buffer, uint32_t width, uint32_t height, uint32_t pitch);

#endif //PPM_H_
