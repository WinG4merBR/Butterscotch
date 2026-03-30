#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/process.h>
#include <sysutil/video_out.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <GL/gl.h>

#include "rsx/rsxutil.h"
#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "gl_legacy_renderer.h"
#include "sys/ps3_time.h"
#include "runner_keyboard.h"
#include "noop_file_system.h"
#include "input/pad_mapping.h"
#include "core/log.h"

#ifndef PS3_DATA_WIN_PATH
#define PS3_DATA_WIN_PATH "/dev_hdd0/game/DEFAULT/USRDIR/data.win"
#endif

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

static double getTimeSeconds(void)
{
    struct timespec ts;
    ps3_clock_gettime(&ts);
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
            .loadTxtrBlobData = true,
            .parseAudo = false, // We don't need to parse audio data yet, because our implementation doesn't support audio yet :3
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .progressCallback = loadingCallback,
            .progressCallbackUserData = NULL,
        });

    return dataWin;
}

static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
    (void)param;

    switch (status)
    {
    // If we didn't handle this callback, the GameOS will restart forcefully
    case SYSUTIL_EXIT_GAME:
    {
        Runner *runner = (Runner *)usrdata;
        if (runner != NULL)
        {
            runner->shouldExit = true;
        }
        // Bye bye :3
        sysProcessExit(0);

        break;
    }

    default:
    {
        break;
    }
    }
}

static Renderer *createPlatformRenderer(DataWin *dataWin, gcmContextData *context)
{
    (void)context;
    fprintf(stderr, "PS3: using PS3GL backend\n");
    Renderer *renderer = GLLegacyRenderer_create();
    if (renderer != NULL && renderer->vtable != NULL && renderer->vtable->init != NULL)
    {
        renderer->vtable->init(renderer, dataWin);
    }
    return renderer;
}

static void preloadRendererRoom(Renderer *renderer, Room *room)
{
    (void)renderer;
    (void)room;
}

static void destroyRenderer(Renderer *renderer)
{
    if (renderer == NULL)
    {
        return;
    }

    if (renderer->vtable != NULL && renderer->vtable->destroy != NULL)
    {
        renderer->vtable->destroy(renderer);
    }
}

int main(void)
{
    sysUtilRegisterCallback(0, sysutil_callback, NULL);

    u16 width, height;
    if (!getResolution(&width, &height))
    {
        width = 1280;
        height = 720;
    }

    ioPadInit(7);

    DataWin *dataWin = loadDataWin();

    if (!dataWin)
    {
        logger("Butterscotch", "FATAL: failed to load data.win");
        return 1;
    }

    // Create a VM and a runner
    logger(dataWin->gen8.displayName, "Creating VM and runner...");
    FileSystem *fs = NoopFileSystem_create();
    VMContext *vm = VM_create(dataWin);
    Runner *runner = Runner_create(dataWin, vm, fs);

    // Attach a renderer
    Renderer *renderer = createPlatformRenderer(dataWin, NULL);
    runner->renderer = renderer;

    // Initialize the first room before entering the main loop
    Runner_initFirstRoom(runner);
    preloadRendererRoom(renderer, runner->currentRoom);

    int32_t lastPreloadedRoomIndex = runner->currentRoomIndex;

    double lastFrameTime = getTimeSeconds();

    // Main loop
    while (!runner->shouldExit)
    {
        sysUtilCheckCallback();

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

        if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10))
        {
            int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");

            runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
        }

        Runner_step(runner);

        if (runner->currentRoomIndex != lastPreloadedRoomIndex)
        {
            preloadRendererRoom(renderer, runner->currentRoom);
            lastPreloadedRoomIndex = runner->currentRoomIndex;
        }

        renderer->vtable->beginFrame(renderer,
                                     dataWin->gen8.defaultWindowWidth,
                                     dataWin->gen8.defaultWindowHeight,
                                     width, height);

        if (runner->drawBackgroundColor)
        {
            uint32_t bg = runner->backgroundColor;
            uint8_t r = (bg) & 0xFF;
            uint8_t g = (bg >> 8) & 0xFF;
            uint8_t b = (bg >> 16) & 0xFF;
            glClearColor((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, 1.0f);
        }
        else
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

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

    logger("Butterscotch", "Exiting main loop, cleaning up...");
    destroyRenderer(renderer);
    DataWin_free(dataWin);
    NoopFileSystem_destroy(fs);
    ioPadEnd();

    return 0;
}
