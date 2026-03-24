#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#include "rsxutils.h"
#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "soft_renderer.h"
#include "compat.h"
#include "runner_keyboard.h"
#include "noop_file_system.h"
#include "include/pad/pad.h"
#include "include/log.h"

#define PS3_DATA_WIN_PATH "/dev_hdd0/game/BTSC00001/USRDIR/data.win"
#define MAX_BUFFERS 2

static const int PAD_MAPPING_COUNT = sizeof(PAD_MAPPINGS) / sizeof(PAD_MAPPINGS[0]);
static bool prevState[sizeof(PAD_MAPPINGS) / sizeof(PAD_MAPPINGS[0])] = {0};

static void loadingCallback(const char *chunkName, int chunkIndex, int totalChunks,
                            DataWin *dataWin, void *userData)
{
    (void)dataWin;
    (void)userData;
    if (chunkName && chunkIndex >= 0 && totalChunks > 0)
    {
        fprintf(stderr, "Loading chunk %d/%d: %s\n", chunkIndex + 1, totalChunks, chunkName);
    }
}

static void clearBuffer(rsxBuffer *buffer, uint32_t color)
{
    uint32_t count = (uint32_t)(buffer->width * buffer->height);
    for (uint32_t i = 0; i < count; i++)
        buffer->ptr[i] = color;
}

typedef struct
{
    uint32_t *pixels;
    int width;
    int height;
    int presentX;
    int presentY;
    int presentW;
    int presentH;
    uint16_t *xMap;
    uint16_t *yMap;
} SoftPresentBuffer;

static bool initSoftPresentBuffer(SoftPresentBuffer *buffer,
                                  int srcW, int srcH,
                                  int dstW, int dstH)
{
    memset(buffer, 0, sizeof(*buffer));

    buffer->width = srcW;
    buffer->height = srcH;
    buffer->pixels = (uint32_t *)memalign(64, (size_t)srcW * (size_t)srcH * sizeof(uint32_t));
    if (!buffer->pixels)
        return false;

    float scaleX = (float)dstW / (float)srcW;
    float scaleY = (float)dstH / (float)srcH;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;

    buffer->presentW = (int)((float)srcW * scale + 0.5f);
    buffer->presentH = (int)((float)srcH * scale + 0.5f);
    buffer->presentX = (dstW - buffer->presentW) / 2;
    buffer->presentY = (dstH - buffer->presentH) / 2;

    buffer->xMap = (uint16_t *)malloc((size_t)buffer->presentW * sizeof(uint16_t));
    buffer->yMap = (uint16_t *)malloc((size_t)buffer->presentH * sizeof(uint16_t));
    if (!buffer->xMap || !buffer->yMap)
        return false;

    for (int x = 0; x < buffer->presentW; x++)
    {
        buffer->xMap[x] = (uint16_t)(((uint32_t)x * (uint32_t)srcW) / (uint32_t)buffer->presentW);
    }

    for (int y = 0; y < buffer->presentH; y++)
    {
        buffer->yMap[y] = (uint16_t)(((uint32_t)y * (uint32_t)srcH) / (uint32_t)buffer->presentH);
    }

    return true;
}

static void destroySoftPresentBuffer(SoftPresentBuffer *buffer)
{
    if (!buffer)
        return;
    free(buffer->pixels);
    free(buffer->xMap);
    free(buffer->yMap);
    memset(buffer, 0, sizeof(*buffer));
}

static void clearPixels(uint32_t *pixels, int width, int height, uint32_t color)
{
    uint32_t count = (uint32_t)(width * height);
    for (uint32_t i = 0; i < count; i++)
    {
        pixels[i] = color;
    }
}

static void presentSoftBuffer(const SoftPresentBuffer *src, rsxBuffer *dst)
{
    for (int y = 0; y < src->presentH; y++)
    {
        uint32_t *dstRow = dst->ptr + (src->presentY + y) * dst->width + src->presentX;
        const uint32_t *srcRow = src->pixels + (int)src->yMap[y] * src->width;

        for (int x = 0; x < src->presentW; x++)
        {
            dstRow[x] = srcRow[src->xMap[x]] | 0xFF000000u;
        }
    }
}

static double getTimeSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

DataWin *loadDataWin()
{
    DataWin *dataWin = DataWin_parse(
        PS3_DATA_WIN_PATH,
        (DataWinParserOptions){
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = true,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = loadingCallback,
            .progressCallbackUserData = NULL,
        });

    return dataWin;
}

int main(void)
{
    void *host_addr = memalign(1024 * 1024, HOST_SIZE);

    if (!host_addr)
    {
        logger("Butterscotch", "FATAL: failed to allocate RSX host memory");
        return 1;
    }
    gcmContextData *context = initScreen(host_addr, HOST_SIZE);

    if (!context)
    {
        logger("Butterscotch", "FATAL: failed to initialize screen");
        return 1;
    }

    u16 width, height;
    getResolution(&width, &height);

    rsxBuffer buffers[MAX_BUFFERS];
    for (int i = 0; i < MAX_BUFFERS; i++)
    {
        makeBuffer(&buffers[i], width, height, i);
        clearBuffer(&buffers[i], 0xFF000000u);
    }

    int currentBuffer = 0;
    flip(context, MAX_BUFFERS - 1);

    ioPadInit(7);

    DataWin *dataWin = loadDataWin();

    if (!dataWin)
    {
        logger("Butterscotch", "FATAL: failed to load data.win");
        return 1;
    }

    SoftPresentBuffer presentBuffer;
    if (!initSoftPresentBuffer(&presentBuffer,
                               (int)dataWin->gen8.defaultWindowWidth,
                               (int)dataWin->gen8.defaultWindowHeight,
                               width,
                               height))
    {
        logger(dataWin->gen8.displayName, "FATAL: failed to initialize present buffer");
        return 1;
    }

    // Create a VM and a runner
    logger(dataWin->gen8.displayName, "Creating VM and runner...");
    FileSystem *fs = NoopFileSystem_create();
    VMContext *vm = VM_create(dataWin);
    Runner *runner = Runner_create(dataWin, vm, fs);

    // Attach a renderer
    Renderer *renderer = SoftRenderer_create(dataWin);
    runner->renderer = renderer;

    // Initialize the first room before entering the main loop
    Runner_initFirstRoom(runner);
    SoftRenderer_preloadRoom(renderer, runner->currentRoom);

    int32_t lastPreloadedRoomIndex = runner->currentRoomIndex;

    double lastFrameTime = getTimeSeconds();

    // Main loop
    while (!runner->shouldExit)
    {

        RunnerKeyboard_beginFrame(runner->keyboard);

        padInfo padinfo;
        ioPadGetInfo(&padinfo);

        if (padinfo.status[0])
        {
            padData paddata;
            ioPadGetData(0, &paddata);

            for (int i = 0; i < PAD_MAPPING_COUNT; i++)
            {
                uint8_t byte = (uint8_t)paddata.button[PAD_MAPPINGS[i].digital];
                uint8_t mask = PAD_MAPPINGS[i].mask;
                int32_t gmlKey = PAD_MAPPINGS[i].gmlKey;

                bool isPressed = (byte & mask) != 0;
                bool wasPressed = prevState[i];

                if (isPressed && !wasPressed)
                {
                    RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                }
                else if (!isPressed && wasPressed)
                {
                    RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
                }

                prevState[i] = isPressed;
            }
        }

        // Reset global interact state because I HATE when I get stuck while moving through rooms
        if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10))
        {
            int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");

            runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
        }

        Runner_step(runner);

        if (runner->currentRoomIndex != lastPreloadedRoomIndex)
        {
            SoftRenderer_preloadRoom(renderer, runner->currentRoom);
            lastPreloadedRoomIndex = runner->currentRoomIndex;
        }

        // Render the frame
        SoftRenderer_setBuffer(renderer,
                               presentBuffer.pixels,
                               presentBuffer.width, presentBuffer.height,
                               dataWin->gen8.defaultWindowWidth,
                               dataWin->gen8.defaultWindowHeight);

        renderer->vtable->beginFrame(renderer,
                                     dataWin->gen8.defaultWindowWidth,
                                     dataWin->gen8.defaultWindowHeight,
                                     presentBuffer.width, presentBuffer.height);

        if (runner->drawBackgroundColor)
        {
            uint32_t bg = runner->backgroundColor;
            uint8_t r = (bg) & 0xFF;
            uint8_t g = (bg >> 8) & 0xFF;
            uint8_t b = (bg >> 16) & 0xFF;
            uint32_t px = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            clearPixels(presentBuffer.pixels, presentBuffer.width, presentBuffer.height, px);
        }
        else
        {
            clearPixels(presentBuffer.pixels, presentBuffer.width, presentBuffer.height, 0xFF000000u);
        }

        Room *room = runner->currentRoom;
        int32_t gameW = dataWin->gen8.defaultWindowWidth;
        int32_t gameH = dataWin->gen8.defaultWindowHeight;
        bool viewsEnabled = (room->flags & 1) != 0;
        bool anyViewRendered = false;

        if (viewsEnabled)
        {
            for (int vi = 0; vi < 8; vi++)
            {
                if (!room->views[vi].enabled)
                    continue;

                RoomView *v = &room->views[vi];
                runner->viewCurrent = vi;
                renderer->vtable->beginView(renderer,
                                            v->viewX, v->viewY, v->viewWidth, v->viewHeight,
                                            v->portX, v->portY, v->portWidth, v->portHeight,
                                            runner->viewAngles[vi]);
                Runner_draw(runner);
                renderer->vtable->endView(renderer);
                anyViewRendered = true;
            }
        }

        if (!anyViewRendered)
        {
            runner->viewCurrent = 0;
            renderer->vtable->beginView(renderer,
                                        0, 0, gameW, gameH,
                                        0, 0, gameW, gameH,
                                        0.0f);
            Runner_draw(runner);
            renderer->vtable->endView(renderer);
        }

        runner->viewCurrent = 0;

        renderer->vtable->endFrame(renderer);
        presentSoftBuffer(&presentBuffer, &buffers[currentBuffer]);

        waitFlip();
        flip(context, buffers[currentBuffer].id);
        currentBuffer ^= 1;

        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / (double)roomSpeed) : (1.0 / 60.0);
        double nextFrameTime = lastFrameTime + targetFrameTime;
        double now = getTimeSeconds();
        double remaining = nextFrameTime - now;

        if (remaining > 0.002)
        {
            usleep((useconds_t)((remaining - 0.001) * 1000000.0));
        }

        while ((now = getTimeSeconds()) < nextFrameTime)
        {
        }

        if (now > nextFrameTime)
        {
            lastFrameTime = now;
        }
        else
        {
            lastFrameTime = nextFrameTime;
        }
    }

    // ===[ Cleanup ]===
    logger("Butterscotch", "Exiting main loop, cleaning up...");
    SoftRenderer_destroy(renderer);
    destroySoftPresentBuffer(&presentBuffer);
    DataWin_free(dataWin);
    NoopFileSystem_destroy(fs);
    ioPadEnd();

    return 0;
}
