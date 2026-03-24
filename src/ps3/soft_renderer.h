#include "renderer.h"
#include "rsx/rsx.h"

typedef struct TexturePageCacheEntry {
    int page_id;
    void *pixels_xdr;
    size_t size;
    int width;
    int height;
    uint64_t last_used;
    void *rsx_handle;
    bool loaded;
} TexturePageCacheEntry;

// Creates a software renderer that blits directly to the RSX framebuffer.
// Must call SoftRenderer_setBuffer() each frame before drawing.
Renderer* SoftRenderer_create(DataWin* dataWin);
void SoftRenderer_setBuffer(Renderer* renderer, uint32_t* fb, int fbWidth, int fbHeight, int gameWidth, int gameHeight);
void SoftRenderer_preloadRoom(Renderer* renderer, Room* room);
void SoftRenderer_TextureCacheInit(void);
void SoftRenderer_TextureCacheDestroy(void);
void SoftRenderer_TextureCachePreloadRoom(DataWin *dw, Room *room);
TexturePageCacheEntry *SoftRenderer_TextureCacheGetPage(DataWin *dw, int page_id);
void SoftRenderer_destroy(Renderer* renderer);
