#ifndef PIXEL_H_
#define PIXEL_H_

#include <stdint.h>
#ifdef USE_RGB565
typedef uint16_t pixel_t;
#else
typedef uint32_t pixel_t;
#endif

#define PITCH_BYTES(width) (sizeof(uint32_t) * ((width * sizeof(pixel_t) + sizeof(uint32_t) - 1) / sizeof(uint32_t)))
#define PITCH_PIXEL_T(width) ((width * sizeof(pixel_t) + sizeof(uint32_t) - 1) / sizeof(uint32_t)) * (sizeof(uint32_t) / sizeof(pixel_t))

#endif //PIXEL_H_
