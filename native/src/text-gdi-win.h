#pragma once

/* GDI text rasteriser. It uses installed system fonts, which keeps the package
 * free of font files and gives font fallback for CJK job names. Each instance
 * must stay on one thread. */

#include "overlay-render.h"

struct bambu_gdi_text;

struct bambu_gdi_text *bambu_gdi_text_create(void);
void bambu_gdi_text_destroy(struct bambu_gdi_text *text);
void bambu_gdi_text_backend(struct bambu_gdi_text *text, struct bambu_text_backend *backend);
