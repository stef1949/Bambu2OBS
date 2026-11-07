#include "bambu-image-win.h"

#include "bambu-log.h"
#include "bambu-zip.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <stdlib.h>

struct bambu_shared_image {
    struct bambu_image image;
    volatile LONG references;
};

static struct bambu_shared_image *copy_frame(IWICBitmapSource *source, UINT width, UINT height)
{
    struct bambu_shared_image *shared = calloc(1, sizeof(*shared));
    UINT stride = width * 4;

    if (shared == NULL)
        return NULL;
    shared->image.pixels = malloc((size_t)stride * height);
    if (shared->image.pixels == NULL ||
        FAILED(IWICBitmapSource_CopyPixels(source, NULL, stride, stride * height, shared->image.pixels))) {
        free(shared->image.pixels);
        free(shared);
        return NULL;
    }
    shared->image.width = (int)width;
    shared->image.height = (int)height;
    shared->references = 1;
    return shared;
}

struct bambu_shared_image *bambu_image_decode(const uint8_t *data, size_t length)
{
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapSource *source;
    struct bambu_shared_image *result = NULL;
    UINT width = 0;
    UINT height = 0;
    UINT target_width;
    UINT target_height;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* Check the signature before handing untrusted bytes to any decoder. */
    if (data == NULL || length == 0 || length > BAMBU_PREVIEW_MAX_IMAGE_BYTES ||
        !bambu_image_signature_ok(data, length))
        goto done;

    if (FAILED(CoCreateInstance(
            &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory
        )))
        goto done;
    if (FAILED(IWICImagingFactory_CreateStream(factory, &stream)))
        goto done;
    if (FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)length)))
        goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(
            factory, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder
        )))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)))
        goto done;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)))
        goto done;
    if (width == 0 || height == 0 || width > BAMBU_IMAGE_MAX_SOURCE_DIMENSION ||
        height > BAMBU_IMAGE_MAX_SOURCE_DIMENSION)
        goto done;

    source = (IWICBitmapSource *)frame;
    target_width = width;
    target_height = height;
    if (width > BAMBU_IMAGE_MAX_DIMENSION || height > BAMBU_IMAGE_MAX_DIMENSION) {
        double scale = (double)BAMBU_IMAGE_MAX_DIMENSION / (double)(width > height ? width : height);
        target_width = (UINT)(width * scale + 0.5);
        target_height = (UINT)(height * scale + 0.5);
        if (target_width == 0)
            target_width = 1;
        if (target_height == 0)
            target_height = 1;
        if (FAILED(IWICImagingFactory_CreateBitmapScaler(factory, &scaler)) ||
            FAILED(IWICBitmapScaler_Initialize(
                scaler, source, target_width, target_height, WICBitmapInterpolationModeFant
            )))
            goto done;
        source = (IWICBitmapSource *)scaler;
    }

    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) ||
        FAILED(IWICFormatConverter_Initialize(
            converter,
            source,
            &GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            NULL,
            0.0,
            WICBitmapPaletteTypeCustom
        )))
        goto done;

    result = copy_frame((IWICBitmapSource *)converter, target_width, target_height);

done:
    if (converter != NULL)
        IWICFormatConverter_Release(converter);
    if (scaler != NULL)
        IWICBitmapScaler_Release(scaler);
    if (frame != NULL)
        IWICBitmapFrameDecode_Release(frame);
    if (decoder != NULL)
        IWICBitmapDecoder_Release(decoder);
    if (stream != NULL)
        IWICStream_Release(stream);
    if (factory != NULL)
        IWICImagingFactory_Release(factory);
    if (SUCCEEDED(com))
        CoUninitialize();
    return result;
}

struct bambu_shared_image *bambu_image_retain(struct bambu_shared_image *image)
{
    if (image != NULL)
        InterlockedIncrement(&image->references);
    return image;
}

void bambu_image_release(struct bambu_shared_image *image)
{
    if (image != NULL && InterlockedDecrement(&image->references) == 0) {
        free(image->image.pixels);
        free(image);
    }
}

const struct bambu_image *bambu_image_pixels(const struct bambu_shared_image *image)
{
    return image != NULL ? &image->image : NULL;
}
