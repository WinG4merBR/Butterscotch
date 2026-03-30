#ifndef RSXUTILS_H
#define RSXUTILS_H

#include <rsx/rsx.h>
#include <ppu-types.h>

#define CB_SIZE   0x100000        /* 1 MB  */
#define HOST_SIZE (32*1024*1024)  /* 32 MB */

typedef struct {
    int      height;
    int      width;
    int      id;
    uint32_t *ptr;
    uint32_t offset;
} rsxBuffer;

void waitFlip(void);
void clearBuffer(rsxBuffer *buffer, uint32_t color);

int rsxFlip(gcmContextData *context, s32 buffer);
int makeBuffer(rsxBuffer *buffer, u16 width, u16 height, int id);
int getResolution(u16 *width, u16 *height);

gcmContextData *initScreen(void *host_addr, u32 size);

void rsxSetRenderTarget(gcmContextData *context, rsxBuffer *buffer);

#endif
