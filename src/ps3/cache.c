#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "renderer.h"
#include "soft_renderer.h"
#include "rsx/rsx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define TEXTURE_CACHE_MAX_ENTRIES 128
#define TEXTURE_CACHE_MAX_BYTES (64 * 1024 * 1024) /* 64 MiB */

typedef struct {
    TexturePageCacheEntry entries[TEXTURE_CACHE_MAX_ENTRIES];
    size_t used_bytes;
    uint64_t tick;
} TexturePageCache;

typedef struct {
    void *pixels;
    size_t size;
    int width;
    int height;
} LoadedTexturePage;

/* Cache global do renderer. Se preferir, mova para dentro do renderer interno. */
static TexturePageCache g_textureCache;

/* Forward declarations */
static void TexturePageCache_init(TexturePageCache *cache);
static void TexturePageCache_destroy(TexturePageCache *cache);
static TexturePageCacheEntry *TexturePageCache_find(TexturePageCache *cache, int page_id);
static TexturePageCacheEntry *TexturePageCache_findFreeSlot(TexturePageCache *cache);
static TexturePageCacheEntry *TexturePageCache_findLRU(TexturePageCache *cache);
static void TexturePageCache_evict(TexturePageCache *cache, TexturePageCacheEntry *entry);
static bool TexturePageCache_ensureSpace(TexturePageCache *cache, size_t bytes_needed);
static bool loadTexturePageFromDataWin(DataWin *dw, int page_id, LoadedTexturePage *out_page);
static TexturePageCacheEntry *TexturePageCache_get(TexturePageCache *cache, DataWin *dw, int page_id);
static void TexturePageCache_preloadRoom(TexturePageCache *cache, DataWin *dw, Room *room);

static inline uint32_t rgbaToArgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static size_t findImageSignatureOffset(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4) {
        return 0;
    }

    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1A\n", 8) == 0) {
        return 0;
    }

    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return 0;
    }

    for (size_t i = 1; (i + 8) <= size; i++) {
        if (memcmp(data + i, "\x89PNG\r\n\x1A\n", 8) == 0) {
            return i;
        }

        if (data[i + 0] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF) {
            return i;
        }
    }

    return 0;
}

static uint32_t estimateNearBlackPercentRgba(const uint8_t *rgba, int w, int h)
{
    if (rgba == NULL || w <= 0 || h <= 0) {
        return 100u;
    }

    int stepX = w / 64;
    int stepY = h / 64;
    if (stepX < 1) {
        stepX = 1;
    }
    if (stepY < 1) {
        stepY = 1;
    }

    uint32_t samples = 0;
    uint32_t nearBlack = 0;
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            const uint8_t *p = &rgba[((size_t)y * (size_t)w + (size_t)x) * 4u];
            if (p[3] == 0) {
                continue;
            }

            samples++;
            if (p[0] <= 8 && p[1] <= 8 && p[2] <= 8) {
                nearBlack++;
            }
        }
    }

    if (samples == 0) {
        return 100u;
    }

    return (nearBlack * 100u) / samples;
}

static void TexturePageCache_preloadPage(TexturePageCache *cache, DataWin *dw, int page_id, uint8_t *seen_pages)
{
    if (dw == NULL || page_id < 0 || (uint32_t)page_id >= dw->txtr.count) {
        return;
    }

    if (seen_pages != NULL) {
        if (seen_pages[page_id] != 0) {
            return;
        }
        seen_pages[page_id] = 1;
    }

    (void)TexturePageCache_get(cache, dw, page_id);
}

static void TexturePageCache_preloadTPAG(TexturePageCache *cache, DataWin *dw, int32_t tpag_index, uint8_t *seen_pages)
{
    if (dw == NULL || tpag_index < 0 || (uint32_t)tpag_index >= dw->tpag.count) {
        return;
    }

    TexturePageItem *tpag = &dw->tpag.items[tpag_index];
    TexturePageCache_preloadPage(cache, dw, (int)tpag->texturePageId, seen_pages);
}

static void TexturePageCache_preloadSprite(TexturePageCache *cache, DataWin *dw, int32_t sprite_index, bool all_frames, uint8_t *seen_pages)
{
    if (dw == NULL || sprite_index < 0 || (uint32_t)sprite_index >= dw->sprt.count) {
        return;
    }

    Sprite *sprite = &dw->sprt.sprites[sprite_index];
    uint32_t frame_count = all_frames ? sprite->textureCount : ((sprite->textureCount > 0) ? 1u : 0u);
    for (uint32_t i = 0; i < frame_count; i++) {
        int32_t tpag_index = DataWin_resolveTPAG(dw, sprite->textureOffsets[i]);
        TexturePageCache_preloadTPAG(cache, dw, tpag_index, seen_pages);
    }
}

/* Opcional: chame isso no create do renderer */
static void TexturePageCache_init(TexturePageCache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

/* Opcional: chame isso no destroy do renderer */
static void TexturePageCache_destroy(TexturePageCache *cache)
{
    for (int i = 0; i < TEXTURE_CACHE_MAX_ENTRIES; i++) {
        TexturePageCacheEntry *entry = &cache->entries[i];

        if (entry->loaded && entry->pixels_xdr) {
            free(entry->pixels_xdr);
            entry->pixels_xdr = NULL;
        }

        entry->rsx_handle = NULL;
        entry->loaded = false;
        entry->size = 0;
        entry->width = 0;
        entry->height = 0;
        entry->page_id = -1;
        entry->last_used = 0;
    }

    cache->used_bytes = 0;
    cache->tick = 0;
}

static TexturePageCacheEntry *TexturePageCache_find(TexturePageCache *cache, int page_id)
{
    for (int i = 0; i < TEXTURE_CACHE_MAX_ENTRIES; i++) {
        TexturePageCacheEntry *entry = &cache->entries[i];

        if (entry->loaded && entry->page_id == page_id) {
            entry->last_used = ++cache->tick;
            return entry;
        }
    }

    return NULL;
}

static TexturePageCacheEntry *TexturePageCache_findFreeSlot(TexturePageCache *cache)
{
    for (int i = 0; i < TEXTURE_CACHE_MAX_ENTRIES; i++) {
        if (!cache->entries[i].loaded) {
            return &cache->entries[i];
        }
    }

    return NULL;
}

static TexturePageCacheEntry *TexturePageCache_findLRU(TexturePageCache *cache)
{
    TexturePageCacheEntry *victim = NULL;

    for (int i = 0; i < TEXTURE_CACHE_MAX_ENTRIES; i++) {
        TexturePageCacheEntry *entry = &cache->entries[i];

        if (!entry->loaded) {
            continue;
        }

        if (victim == NULL || entry->last_used < victim->last_used) {
            victim = entry;
        }
    }

    return victim;
}

static void TexturePageCache_evict(TexturePageCache *cache, TexturePageCacheEntry *entry)
{
    if (entry == NULL || !entry->loaded) {
        return;
    }

    if (entry->pixels_xdr != NULL) {
        free(entry->pixels_xdr);
        entry->pixels_xdr = NULL;
    }

    entry->rsx_handle = NULL;

    if (cache->used_bytes >= entry->size) {
        cache->used_bytes -= entry->size;
    } else {
        cache->used_bytes = 0;
    }

    memset(entry, 0, sizeof(*entry));
    entry->page_id = -1;
}

static bool TexturePageCache_ensureSpace(TexturePageCache *cache, size_t bytes_needed)
{
    if (bytes_needed > TEXTURE_CACHE_MAX_BYTES) {
        return false;
    }

    while ((cache->used_bytes + bytes_needed) > TEXTURE_CACHE_MAX_BYTES) {
        TexturePageCacheEntry *victim = TexturePageCache_findLRU(cache);
        if (victim == NULL) {
            return false;
        }

        TexturePageCache_evict(cache, victim);
    }

    return true;
}

static TexturePageCacheEntry *TexturePageCache_get(TexturePageCache *cache, DataWin *dw, int page_id)
{
    TexturePageCacheEntry *entry = TexturePageCache_find(cache, page_id);
    if (entry != NULL) {
        return entry;
    }

    LoadedTexturePage loaded;
    memset(&loaded, 0, sizeof(loaded));

    if (!loadTexturePageFromDataWin(dw, page_id, &loaded)) {
        return NULL;
    }

    if (loaded.pixels == NULL || loaded.size == 0) {
        if (loaded.pixels != NULL) {
            free(loaded.pixels);
        }
        return NULL;
    }

    if (!TexturePageCache_ensureSpace(cache, loaded.size)) {
        free(loaded.pixels);
        return NULL;
    }

    TexturePageCacheEntry *slot = TexturePageCache_findFreeSlot(cache);
    if (slot == NULL) {
        TexturePageCacheEntry *victim = TexturePageCache_findLRU(cache);
        if (victim == NULL) {
            free(loaded.pixels);
            return NULL;
        }

        TexturePageCache_evict(cache, victim);
        slot = victim;
    }

    slot->page_id = page_id;
    slot->pixels_xdr = loaded.pixels;
    slot->size = loaded.size;
    slot->width = loaded.width;
    slot->height = loaded.height;
    slot->last_used = ++cache->tick;
    slot->rsx_handle = NULL;
    slot->loaded = true;

    cache->used_bytes += loaded.size;

    return slot;
}

static bool loadTexturePageFromDataWin(DataWin *dw, int page_id, LoadedTexturePage *out_page)
{
    memset(out_page, 0, sizeof(*out_page));

    if (dw == NULL || page_id < 0 || (uint32_t)page_id >= dw->txtr.count) {
        return false;
    }

    Texture *texture = &dw->txtr.textures[page_id];
    if (texture->blobData == NULL || texture->blobSize == 0) {
        return false;
    }

    size_t sig_offset = findImageSignatureOffset(texture->blobData, (size_t)texture->blobSize);
    size_t decode_size = (size_t)texture->blobSize - sig_offset;
    if (decode_size == 0 || decode_size > (size_t)INT_MAX) {
        return false;
    }

    const size_t max_pixels = (size_t)4096 * (size_t)4096;

    int info_w = 0;
    int info_h = 0;
    int info_ch = 0;
    if (!stbi_info_from_memory(texture->blobData + sig_offset, (int)decode_size, &info_w, &info_h, &info_ch)) {
        return false;
    }

    if (info_w <= 0 || info_h <= 0) {
        return false;
    }

    if ((size_t)info_w * (size_t)info_h > max_pixels) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    uint8_t *rgba = stbi_load_from_memory(texture->blobData + sig_offset,
                                          (int)decode_size,
                                          &width,
                                          &height,
                                          &channels,
                                          4);
    (void)channels;
    if (rgba == NULL || width <= 0 || height <= 0) {
        if (rgba != NULL) {
            stbi_image_free(rgba);
        }
        return false;
    }

    uint32_t best_near_black = estimateNearBlackPercentRgba(rgba, width, height);
    size_t best_offset = sig_offset;

    if (best_near_black >= 85u) {
        for (size_t off = sig_offset + 1; off + 8 <= (size_t)texture->blobSize; off++) {
            bool is_png = memcmp(texture->blobData + off, "\x89PNG\r\n\x1A\n", 8) == 0;
            bool is_jpg = texture->blobData[off + 0] == 0xFF &&
                          texture->blobData[off + 1] == 0xD8 &&
                          texture->blobData[off + 2] == 0xFF;
            if (!is_png && !is_jpg) {
                continue;
            }

            size_t alt_size = (size_t)texture->blobSize - off;
            if (alt_size > (size_t)INT_MAX) {
                alt_size = (size_t)INT_MAX;
            }

            int alt_w = 0;
            int alt_h = 0;
            int alt_ch = 0;
            uint8_t *alt_rgba = stbi_load_from_memory(texture->blobData + off, (int)alt_size, &alt_w, &alt_h, &alt_ch, 4);
            if (alt_rgba == NULL || alt_w <= 0 || alt_h <= 0) {
                if (alt_rgba != NULL) {
                    stbi_image_free(alt_rgba);
                }
                continue;
            }

            uint32_t alt_near_black = estimateNearBlackPercentRgba(alt_rgba, alt_w, alt_h);
            if (alt_near_black + 20u < best_near_black) {
                stbi_image_free(rgba);
                rgba = alt_rgba;
                width = alt_w;
                height = alt_h;
                best_near_black = alt_near_black;
                best_offset = off;
            } else {
                stbi_image_free(alt_rgba);
            }
        }
    }

    if (best_offset != sig_offset) {
        fprintf(stderr,
                "TextureCache: texture %d switched decode signature offset %u -> %u (nearBlack=%u%%)\n",
                page_id,
                (unsigned)sig_offset,
                (unsigned)best_offset,
                best_near_black);
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > (SIZE_MAX / sizeof(uint32_t))) {
        stbi_image_free(rgba);
        return false;
    }

    uint32_t *argb = (uint32_t *)rgba;
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        argb[i] = rgbaToArgb(r, g, b, a);
    }

    out_page->pixels = argb;
    out_page->size = pixel_count * sizeof(uint32_t);
    out_page->width = width;
    out_page->height = height;
    return true;
}

static void TexturePageCache_preloadRoom(TexturePageCache *cache, DataWin *dw, Room *room)
{
    if (cache == NULL || dw == NULL || room == NULL || dw->txtr.count == 0) {
        return;
    }

    uint8_t *seen_pages = (uint8_t *)calloc(dw->txtr.count, sizeof(uint8_t));
    if (seen_pages == NULL) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        RoomBackground *room_bg = &room->backgrounds[i];
        if (!room_bg->enabled) {
            continue;
        }
        if (room_bg->backgroundDefinition < 0 || (uint32_t)room_bg->backgroundDefinition >= dw->bgnd.count) {
            continue;
        }

        Background *bg = &dw->bgnd.backgrounds[room_bg->backgroundDefinition];
        int32_t tpag_index = DataWin_resolveTPAG(dw, bg->textureOffset);
        TexturePageCache_preloadTPAG(cache, dw, tpag_index, seen_pages);
    }

    for (uint32_t i = 0; i < room->tileCount; i++) {
        RoomTile *tile = &room->tiles[i];
        if (tile->backgroundDefinition < 0 || (uint32_t)tile->backgroundDefinition >= dw->bgnd.count) {
            continue;
        }

        Background *bg = &dw->bgnd.backgrounds[tile->backgroundDefinition];
        int32_t tpag_index = DataWin_resolveTPAG(dw, bg->textureOffset);
        TexturePageCache_preloadTPAG(cache, dw, tpag_index, seen_pages);
    }

    for (uint32_t i = 0; i < room->gameObjectCount; i++) {
        RoomGameObject *room_obj = &room->gameObjects[i];
        if (room_obj->objectDefinition < 0 || (uint32_t)room_obj->objectDefinition >= dw->objt.count) {
            continue;
        }

        GameObject *obj = &dw->objt.objects[room_obj->objectDefinition];
        if (obj->spriteId < 0 || (uint32_t)obj->spriteId >= dw->sprt.count) {
            continue;
        }

        Sprite *sprite = &dw->sprt.sprites[obj->spriteId];
        TexturePageCache_preloadSprite(cache, dw, obj->spriteId, sprite->preload, seen_pages);
    }

    free(seen_pages);
}

void SoftRenderer_TextureCacheInit(void)
{
    TexturePageCache_init(&g_textureCache);
}

void SoftRenderer_TextureCacheDestroy(void)
{
    TexturePageCache_destroy(&g_textureCache);
}

void SoftRenderer_TextureCachePreloadRoom(DataWin *dw, Room *room)
{
    TexturePageCache_preloadRoom(&g_textureCache, dw, room);
}

TexturePageCacheEntry *SoftRenderer_TextureCacheGetPage(DataWin *dw, int page_id)
{
    return TexturePageCache_get(&g_textureCache, dw, page_id);
}
