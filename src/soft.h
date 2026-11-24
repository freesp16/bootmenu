/* The CPU renderer (soft.c), behind gfx.c's functions when the backend is "cpu". */
#ifndef SOFT_H
#define SOFT_H

#include <stddef.h>
#include <stdint.h>

int soft_open(char *err, size_t errlen);
const char *soft_info(void);
void soft_begin(uint32_t rgb);
void soft_clip(int x, int y, int w, int h);
void soft_rect(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom, float border,
               float soft);
void soft_glyph(const uint8_t *atlas, int atlas_w, int sx, int sy, int w, int h, int dx, int dy, uint32_t rgba);
int soft_image_create(const uint8_t *rgba, int w, int h);
void soft_image(int handle, float x, float y, float w, float h, float alpha);
int soft_end(const char *shot); /* writes `shot` (a PPM) first when given */

#endif
