#include "text-gdi-win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FONT_CACHE_SIZE 16
#define MAX_TEXT_CHARS 512

struct font_entry {
    HFONT font;
    int height;
    bool bold;
    unsigned last_used;
};

struct bambu_gdi_text {
    HDC dc;
    HBITMAP bitmap;
    void *bits;
    int width;
    int height;
    struct font_entry fonts[FONT_CACHE_SIZE];
    unsigned clock;
};

struct bambu_gdi_text *bambu_gdi_text_create(void)
{
    struct bambu_gdi_text *text = calloc(1, sizeof(*text));

    if (text == NULL)
        return NULL;
    text->dc = CreateCompatibleDC(NULL);
    if (text->dc == NULL) {
        free(text);
        return NULL;
    }
    SetBkMode(text->dc, TRANSPARENT);
    SetTextColor(text->dc, RGB(255, 255, 255));
    return text;
}

void bambu_gdi_text_destroy(struct bambu_gdi_text *text)
{
    if (text == NULL)
        return;
    if (text->dc != NULL)
        SelectObject(text->dc, GetStockObject(SYSTEM_FONT));
    for (int i = 0; i < FONT_CACHE_SIZE; i++) {
        if (text->fonts[i].font != NULL)
            DeleteObject(text->fonts[i].font);
    }
    if (text->bitmap != NULL)
        DeleteObject(text->bitmap);
    if (text->dc != NULL)
        DeleteDC(text->dc);
    free(text);
}

static HFONT get_font(struct bambu_gdi_text *text, float size_px, bool bold)
{
    int height = (int)(size_px + 0.5f);
    struct font_entry *oldest = &text->fonts[0];

    if (height < 6)
        height = 6;
    text->clock++;
    for (int i = 0; i < FONT_CACHE_SIZE; i++) {
        struct font_entry *entry = &text->fonts[i];
        if (entry->font != NULL && entry->height == height && entry->bold == bold) {
            entry->last_used = text->clock;
            return entry->font;
        }
        if (entry->last_used < oldest->last_used)
            oldest = entry;
    }

    /* The victim may be selected; deselect before deleting it. */
    SelectObject(text->dc, GetStockObject(SYSTEM_FONT));
    if (oldest->font != NULL)
        DeleteObject(oldest->font);
    oldest->font = CreateFontW(
        -height,
        0,
        0,
        0,
        /* The Semibold face is already heavy; asking for more would synthesise it. */
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        bold ? L"Segoe UI Semibold" : L"Segoe UI"
    );
    oldest->height = height;
    oldest->bold = bold;
    oldest->last_used = text->clock;
    return oldest->font;
}

static bool ensure_bitmap(struct bambu_gdi_text *text, int width, int height)
{
    BITMAPINFO info;

    if (text->bitmap != NULL && text->width >= width && text->height >= height)
        return true;
    if (text->bitmap != NULL) {
        SelectObject(text->dc, GetStockObject(DC_BRUSH));
        DeleteObject(text->bitmap);
        text->bitmap = NULL;
    }

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    text->bitmap = CreateDIBSection(text->dc, &info, DIB_RGB_COLORS, &text->bits, NULL, 0);
    if (text->bitmap == NULL)
        return false;
    SelectObject(text->dc, text->bitmap);
    text->width = width;
    text->height = height;
    return true;
}

static int to_wide(const char *utf8, wchar_t *out, int capacity)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, capacity);

    if (length == 0) {
        /* Over-long text: keep what fits rather than showing nothing. */
        length = MultiByteToWideChar(CP_UTF8, 0, utf8, capacity / 2, out, capacity - 1);
        out[length] = L'\0';
        return length;
    }
    return length - 1;
}

static void rasterize(
    void *context,
    const char *utf8,
    float size_px,
    bool bold,
    enum bambu_text_align align,
    uint8_t *mask,
    int width,
    int height
)
{
    struct bambu_gdi_text *text = context;
    wchar_t wide[MAX_TEXT_CHARS];
    int length = to_wide(utf8, wide, MAX_TEXT_CHARS);
    HFONT font = get_font(text, size_px, bold);
    RECT rect = {0, 0, width, height};
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
    const uint8_t *row;

    if (length <= 0 || font == NULL || !ensure_bitmap(text, width, height))
        return;

    memset(text->bits, 0, (size_t)text->width * (size_t)text->height * 4);
    SelectObject(text->dc, font);
    if (align == BAMBU_ALIGN_CENTER)
        format |= DT_CENTER;
    else if (align == BAMBU_ALIGN_RIGHT)
        format |= DT_RIGHT;
    DrawTextW(text->dc, wide, length, &rect, format);
    GdiFlush();

    /* White on black means any colour channel is the coverage value. */
    row = text->bits;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            mask[(size_t)y * (size_t)width + (size_t)x] = row[(size_t)x * 4 + 1];
        row += (size_t)text->width * 4;
    }
}

static float measure(void *context, const char *utf8, float size_px, bool bold)
{
    struct bambu_gdi_text *text = context;
    wchar_t wide[MAX_TEXT_CHARS];
    int length = to_wide(utf8, wide, MAX_TEXT_CHARS);
    HFONT font = get_font(text, size_px, bold);
    SIZE size;

    if (length <= 0 || font == NULL)
        return 0.0f;
    SelectObject(text->dc, font);
    if (!GetTextExtentPoint32W(text->dc, wide, length, &size))
        return 0.0f;
    return (float)size.cx;
}

void bambu_gdi_text_backend(struct bambu_gdi_text *text, struct bambu_text_backend *backend)
{
    backend->context = text;
    backend->rasterize = rasterize;
    backend->measure = measure;
}
