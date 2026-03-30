#include <ppu-lv2.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sysutil/video_out.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <time.h>
#include <math.h>

#include "rsx/rsxutil.h"

#define GCM_LABEL_INDEX 255

static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

static void waitRSXIdle(gcmContextData *context);

void waitFlip()
{
    while (gcmGetFlipStatus() != 0)
        usleep(200);
    gcmResetFlipStatus();
}

void clearBuffer(rsxBuffer *buffer, uint32_t color)
{
    uint32_t count;

    if (buffer == NULL || buffer->ptr == NULL)
    {
        return;
    }

    count = (uint32_t)(buffer->width * buffer->height);
    for (uint32_t i = 0; i < count; i++)
        buffer->ptr[i] = color;
}

int rsxFlip(gcmContextData *context, s32 buffer)
{
    if (gcmSetFlip(context, buffer) == 0) {
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
        return 1;
    }
    return 0;
}

int makeBuffer(rsxBuffer *buffer, u16 width, u16 height, int id)
{
    int depth = sizeof(u32);
    int pitch = depth * width;
    int size  = depth * width * height;

    buffer->ptr = (uint32_t *)rsxMemalign(64, size);
    if (!buffer->ptr) goto error;

    if (rsxAddressToOffset(buffer->ptr, &buffer->offset) != 0) goto error;
    if (gcmSetDisplayBuffer(id, buffer->offset, pitch, width, height) != 0) goto error;

    buffer->width  = width;
    buffer->height = height;
    buffer->id     = id;
    return 1;

error:
    if (buffer->ptr) rsxFree(buffer->ptr);
    return 0;
}

int getResolution(u16 *width, u16 *height)
{
    videoOutState      state;
    videoOutResolution resolution;

    if (videoOutGetState(0, 0, &state) == 0 &&
        videoOutGetResolution(state.displayMode.resolution, &resolution) == 0)
    {
        if (width)  *width  = resolution.width;
        if (height) *height = resolution.height;
        return 1;
    }
    return 0;
}

gcmContextData *initScreen(void *host_addr, u32 size)
{
    gcmContextData    *context = NULL;
    videoOutState         state;
    videoOutState         postState;
    videoOutConfiguration vconfig;
    videoOutConfiguration fallbackConfig;
    u32                targetResolution;
    videoOutResolution    res;
    int                configureRes;

    rsxInit(&context, CB_SIZE, size, host_addr);
    if (!context) goto error;

    if (videoOutGetState(0, 0, &state) != 0) goto error;
    if (state.state != 0)               goto error;
    if (videoOutGetResolution(state.displayMode.resolution, &res) != 0) goto error;

    targetResolution = VIDEO_OUT_RESOLUTION_720;
    if (videoOutGetResolution(targetResolution, &res) != 0) {
        targetResolution = state.displayMode.resolution;
        if (videoOutGetResolution(targetResolution, &res) != 0) goto error;
    }

    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = targetResolution;
    vconfig.format     = VIDEO_OUT_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = res.width * sizeof(u32);
    vconfig.aspect     = state.displayMode.aspect;

    memset(&fallbackConfig, 0, sizeof(fallbackConfig));
    fallbackConfig.resolution = state.displayMode.resolution;
    fallbackConfig.format     = VIDEO_OUT_BUFFER_FORMAT_XRGB;
    fallbackConfig.pitch      = res.width * sizeof(u32);
    fallbackConfig.aspect     = state.displayMode.aspect;

    waitRSXIdle(context);

    configureRes = videoOutConfigure(0, &vconfig, NULL, 0);
    if (configureRes != 0) {
        if (videoOutGetResolution(fallbackConfig.resolution, &res) != 0) goto error;
        fallbackConfig.pitch = res.width * sizeof(u32);
        if (videoOutConfigure(0, &fallbackConfig, NULL, 0) != 0) goto error;
    }
    if (videoOutGetState(0, 0, &postState) != 0) goto error;
    if (videoOutGetResolution(postState.displayMode.resolution, &res) != 0) goto error;

    fprintf(stderr, "PS3: video mode %ux%u\n", res.width, res.height);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    depth_pitch  = res.width * sizeof(u32);
    depth_buffer = (u32 *)rsxMemalign(64, res.height * depth_pitch * 2);
    rsxAddressToOffset(depth_buffer, &depth_offset);
    gcmResetFlipStatus();

    return context;

error:
    if (context)   rsxFinish(context, 0);
    if (host_addr) free(host_addr);
    return NULL;
}

static void waitFinish(gcmContextData *context, u32 sLabelVal)
{
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
    rsxFlushBuffer(context);
    while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal)
        usleep(30);
    sLabelVal++;
}

static void waitRSXIdle(gcmContextData *context)
{
    u32 sLabelVal = 1;
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
    rsxSetWaitLabel(context, GCM_LABEL_INDEX, sLabelVal);
    sLabelVal++;
    waitFinish(context, sLabelVal);
}

void rsxSetRenderTarget(gcmContextData *context, rsxBuffer *buffer)
{
    gcmSurface sf;
    sf.colorFormat     = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget     = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0]= GCM_LOCATION_RSX;
    sf.colorOffset[0]  = buffer->offset;
    sf.colorPitch[0]   = depth_pitch;
    sf.colorLocation[1]= GCM_LOCATION_RSX;
    sf.colorLocation[2]= GCM_LOCATION_RSX;
    sf.colorLocation[3]= GCM_LOCATION_RSX;
    sf.colorOffset[1]  = sf.colorOffset[2] = sf.colorOffset[3] = 0;
    sf.colorPitch[1]   = sf.colorPitch[2]  = sf.colorPitch[3]  = 64;
    sf.depthFormat     = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation   = GCM_LOCATION_RSX;
    sf.depthOffset     = depth_offset;
    sf.depthPitch      = depth_pitch;
    sf.type            = GCM_TEXTURE_LINEAR;
    sf.antiAlias       = GCM_SURFACE_CENTER_1;
    sf.width           = buffer->width;
    sf.height          = buffer->height;
    sf.x = sf.y        = 0;
    rsxSetSurface(context, &sf);
}
