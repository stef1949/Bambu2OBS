#pragma once

/* Dependency-free PNG writer for test output, using stored deflate blocks. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t png_crc(const uint8_t *data, size_t length, uint32_t crc)
{
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static void png_put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void png_chunk(FILE *file, const char *type, const uint8_t *data, size_t length)
{
    uint8_t header[8];
    uint8_t tail[4];
    uint32_t crc;

    png_put32(header, (uint32_t)length);
    for (int i = 0; i < 4; i++)
        header[4 + i] = (uint8_t)type[i];
    crc = png_crc(header + 4, 4, 0xFFFFFFFFu);
    crc = png_crc(data, length, crc);
    png_put32(tail, ~crc);
    fwrite(header, 1, 8, file);
    fwrite(data, 1, length, file);
    fwrite(tail, 1, 4, file);
}

/* Writes straight-alpha RGBA rows. Returns 0 on success. */
static int png_write_rgba(const char *path, const uint8_t *rgba, int width, int height)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    size_t row_bytes = (size_t)width * 4 + 1;
    size_t raw_size = row_bytes * (size_t)height;
    size_t blocks = (raw_size + 65534) / 65535;
    uint8_t *raw = malloc(raw_size);
    uint8_t *idat = malloc(raw_size + blocks * 5 + 6);
    uint8_t ihdr[13];
    FILE *file;
    size_t offset = 0;
    size_t used = 0;
    uint32_t a = 1;
    uint32_t b = 0;

    if (raw == NULL || idat == NULL)
        return 1;
    for (int y = 0; y < height; y++) {
        raw[(size_t)y * row_bytes] = 0;
        memcpy(raw + (size_t)y * row_bytes + 1, rgba + (size_t)y * (size_t)width * 4, (size_t)width * 4);
    }

    idat[used++] = 0x78;
    idat[used++] = 0x01;
    while (offset < raw_size) {
        size_t chunk = raw_size - offset > 65535 ? 65535 : raw_size - offset;
        idat[used++] = offset + chunk == raw_size ? 1 : 0;
        idat[used++] = (uint8_t)chunk;
        idat[used++] = (uint8_t)(chunk >> 8);
        idat[used++] = (uint8_t)~chunk;
        idat[used++] = (uint8_t)(~chunk >> 8);
        memcpy(idat + used, raw + offset, chunk);
        used += chunk;
        offset += chunk;
    }
    for (size_t i = 0; i < raw_size; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    png_put32(idat + used, (b << 16) | a);
    used += 4;

    png_put32(ihdr, (uint32_t)width);
    png_put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    file = fopen(path, "wb");
    if (file == NULL) {
        free(raw);
        free(idat);
        return 1;
    }
    fwrite(signature, 1, sizeof(signature), file);
    png_chunk(file, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(file, "IDAT", idat, used);
    png_chunk(file, "IEND", NULL, 0);
    fclose(file);
    free(raw);
    free(idat);
    return 0;
}
