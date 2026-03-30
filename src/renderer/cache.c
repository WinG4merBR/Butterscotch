#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "core/utils.h"
#include "renderer.h"
#include "soft_renderer.h"
#include "txtr_pack_format.h"

#ifdef __PPU__
    #include <lv2/sysfs.h>
#endif

#define TEXTURE_CACHE_MAX_ENTRIES 192
#define TEXTURE_CACHE_MAX_BYTES (96 * 1024 * 1024) /* 96 MiB */
#define TEXTURE_PACK_FULLY_LOADED_MAX_BYTES (160 * 1024 * 1024) /* 160 MiB */

#ifdef __PPU__
    #define RSX_MAIN_MEMORY_MAP_ALIGNMENT (1024u * 1024u)
#endif

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

typedef struct {
    char* path;
    uint8_t* entries;
    uint32_t entry_count;
    uint8_t* file_data;
    uint64_t file_size;
    TexturePageCacheEntry* direct_entries;
    bool initialized;
    bool available;
    bool fully_loaded;
#ifdef __PPU__
    s32 fd;
#else
    FILE* file;
#endif
    bool handle_open;
} TexturePackIndex;

static TexturePageCache g_textureCache;
static TexturePackIndex g_texturePack;
static uint32_t g_textureCacheInitCount = 0u;

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
static void TexturePackIndex_destroy(TexturePackIndex* pack);
static bool TexturePackIndex_ensureLoaded(TexturePackIndex* pack, DataWin* dw);
static bool TexturePackIndex_readBytesAt(TexturePackIndex* pack, uint64_t offset, void* dest, size_t size);
static TexturePageCacheEntry* TexturePackIndex_getDirectEntry(TexturePackIndex* pack, DataWin* dw, int page_id);
static bool loadTexturePageFromPack(DataWin* dw, int page_id, LoadedTexturePage* out_page);
static bool TexturePackIndex_tryFullyLoad(TexturePackIndex* pack);

static inline uint32_t rgbaToArgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static size_t alignUpSize(size_t value, size_t alignment)
{
    if (alignment == 0u)
    {
        return value;
    }

    size_t remainder = value % alignment;
    if (remainder == 0u)
    {
        return value;
    }

    return value + (alignment - remainder);
}

static void convertRgbaBytesToArgb(uint8_t* rgba, size_t pixel_count)
{
    uint32_t* argb = (uint32_t*)rgba;
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        argb[i] = rgbaToArgb(r, g, b, a);
    }
}

static char* buildCompanionPath(const char* sourcePath, const char* filename)
{
    if (sourcePath == NULL || filename == NULL) {
        return NULL;
    }

    const char* slash = strrchr(sourcePath, '/');
    if (slash == NULL) {
        return safeStrdup(filename);
    }

    size_t dirLen = (size_t)(slash - sourcePath) + 1u;
    size_t fileLen = strlen(filename);
    char* result = safeMalloc(dirLen + fileLen + 1u);
    memcpy(result, sourcePath, dirLen);
    memcpy(result + dirLen, filename, fileLen + 1u);
    return result;
}

static void TexturePackIndex_destroy(TexturePackIndex* pack)
{
    if (pack == NULL) {
        return;
    }

#ifdef __PPU__
    if (pack->handle_open) {
        sysFsClose(pack->fd);
    }
#else
    if (pack->handle_open && pack->file != NULL) {
        fclose(pack->file);
    }
#endif

    free(pack->path);
    free(pack->entries);
    free(pack->file_data);
    free(pack->direct_entries);
    memset(pack, 0, sizeof(*pack));
}

static bool TexturePackIndex_ensureLoaded(TexturePackIndex* pack, DataWin* dw)
{
    if (pack->initialized) {
        return pack->available;
    }

    pack->initialized = true;
    if (dw == NULL || dw->sourcePath == NULL) {
        return false;
    }

    pack->path = buildCompanionPath(dw->sourcePath, TXTR_PACK_FILENAME);
    if (pack->path == NULL) {
        return false;
    }

#ifdef __PPU__
    if (sysFsOpen(pack->path, SYS_O_RDONLY, &pack->fd, NULL, 0) != 0) {
        return false;
    }
    sysFSStat st;
    if (sysFsFstat(pack->fd, &st) != 0) {
        return false;
    }
    pack->file_size = (uint64_t)st.size;
#else
    pack->file = fopen(pack->path, "rb");
    if (pack->file == NULL) {
        return false;
    }
    if (fseek(pack->file, 0, SEEK_END) != 0) {
        return false;
    }
    long fileSize = ftell(pack->file);
    if (fileSize < 0 || fseek(pack->file, 0, SEEK_SET) != 0) {
        return false;
    }
    pack->file_size = (uint64_t)fileSize;
#endif
    pack->handle_open = true;

    uint8_t header[TXTR_PACK_HEADER_SIZE];
    if (!TexturePackIndex_readBytesAt(pack, 0, header, sizeof(header))) {
        return false;
    }

    if (memcmp(header, TXTR_PACK_MAGIC, TXTR_PACK_MAGIC_SIZE) != 0) {
        fprintf(stderr, "TextureCache: ignoring invalid texture pack magic in %s\n", pack->path);
        return false;
    }

    uint32_t version = TxtrPack_readU32LE(header + 8);
    if (version != TXTR_PACK_VERSION) {
        fprintf(stderr,
                "TextureCache: ignoring texture pack %s with unsupported version %u\n",
                pack->path,
                version);
        return false;
    }

    pack->entry_count = TxtrPack_readU32LE(header + 12);
    if (pack->entry_count != dw->txtr.count) {
        fprintf(stderr,
                "TextureCache: ignoring texture pack %s because entry count %u != txtr.count %u\n",
                pack->path,
                pack->entry_count,
                dw->txtr.count);
        return false;
    }

    if (pack->entry_count > 0) {
        size_t entriesSize = (size_t)pack->entry_count * (size_t)TXTR_PACK_ENTRY_SIZE;
        pack->entries = safeMalloc(entriesSize);
        if (!TexturePackIndex_readBytesAt(pack, TXTR_PACK_HEADER_SIZE, pack->entries, entriesSize)) {
            fprintf(stderr, "TextureCache: failed to read texture pack index from %s\n", pack->path);
            free(pack->entries);
            pack->entries = NULL;
            return false;
        }
    }

    if (pack->file_size > 0 && pack->file_size <= (uint64_t)TEXTURE_PACK_FULLY_LOADED_MAX_BYTES) {
        if (!TexturePackIndex_tryFullyLoad(pack)) {
            fprintf(stderr,
                    "TextureCache: not enough memory to fully load %s (%llu bytes), falling back to streamed mode\n",
                    pack->path,
                    (unsigned long long)pack->file_size);
        }
    }

    pack->available = true;
    if (pack->fully_loaded) {
        fprintf(stderr,
                "TextureCache: fully loaded cooked texture pack %s into RAM (%u pages, %llu bytes)\n",
                pack->path,
                pack->entry_count,
                (unsigned long long)pack->file_size);
    } else {
        fprintf(stderr,
                "TextureCache: using streamed cooked texture pack %s (%u pages, %llu bytes)\n",
                pack->path,
                pack->entry_count,
                (unsigned long long)pack->file_size);
    }
    return true;
}

static bool TexturePackIndex_tryFullyLoad(TexturePackIndex* pack)
{
    uint8_t* fileData;
    TexturePageCacheEntry* directEntries;

    if (pack == NULL || !pack->handle_open || pack->file_size == 0 || pack->entry_count == 0) {
        return false;
    }

    fileData = (uint8_t*)malloc((size_t)pack->file_size);
    if (fileData == NULL) {
        return false;
    }

    if (!TexturePackIndex_readBytesAt(pack, 0, fileData, (size_t)pack->file_size)) {
        fprintf(stderr, "TextureCache: failed to fully load texture pack %s\n", pack->path);
        free(fileData);
        return false;
    }

    directEntries = (TexturePageCacheEntry*)calloc(pack->entry_count, sizeof(TexturePageCacheEntry));
    if (directEntries == NULL) {
        free(fileData);
        return false;
    }

    for (uint32_t pageId = 0; pageId < pack->entry_count; pageId++) {
        const uint8_t* entry = pack->entries + ((size_t)pageId * (size_t)TXTR_PACK_ENTRY_SIZE);
        uint32_t width = TxtrPack_readU32LE(entry + 0);
        uint32_t height = TxtrPack_readU32LE(entry + 4);
        uint64_t dataOffset = TxtrPack_readU64LE(entry + 8);
        uint64_t dataSize = TxtrPack_readU64LE(entry + 16);
        uint32_t format = TxtrPack_readU32LE(entry + 32);
        uint32_t flags = TxtrPack_readU32LE(entry + 36);

        if ((flags & TXTR_PACK_ENTRY_FLAG_PRESENT) == 0u) {
            continue;
        }

        size_t pixelCount = (size_t)width * (size_t)height;
        if ((format != TXTR_PACK_PIXEL_FORMAT_RGBA8 && format != TXTR_PACK_PIXEL_FORMAT_ARGB8) ||
            width == 0 ||
            height == 0 ||
            pixelCount == 0 ||
            pixelCount > (SIZE_MAX / 4u) ||
            dataSize != (uint64_t)(pixelCount * 4u) ||
            dataOffset > pack->file_size ||
            dataSize > (pack->file_size - dataOffset)) {
            fprintf(stderr, "TextureCache: invalid fully-loaded texture pack entry for page %u\n", pageId);
            free(fileData);
            free(directEntries);
            return false;
        }

        uint8_t* pageBytes = fileData + dataOffset;
        if (format == TXTR_PACK_PIXEL_FORMAT_RGBA8) {
            convertRgbaBytesToArgb(pageBytes, pixelCount);
        }

        TexturePageCacheEntry* direct = &directEntries[pageId];
        direct->page_id = (int)pageId;
        direct->pixels_xdr = pageBytes;
        direct->size = (size_t)dataSize;
        direct->width = (int)width;
        direct->height = (int)height;
        direct->loaded = true;
    }

#ifdef __PPU__
    sysFsClose(pack->fd);
#else
    fclose(pack->file);
    pack->file = NULL;
#endif
    pack->handle_open = false;
    pack->file_data = fileData;
    pack->direct_entries = directEntries;
    pack->fully_loaded = true;
    return true;
}

static bool TexturePackIndex_readBytesAt(TexturePackIndex* pack, uint64_t offset, void* dest, size_t size)
{
    if (pack == NULL || !pack->handle_open || dest == NULL) {
        return false;
    }

#ifdef __PPU__
    u64 pos = 0;
    if (sysFsLseek(pack->fd, (s64)offset, SEEK_SET, &pos) != 0) {
        return false;
    }

    size_t total = 0;
    while (total < size) {
        u64 nread __attribute__((aligned(8))) = 0;
        if (sysFsRead(pack->fd, (uint8_t*)dest + total, (u64)(size - total), &nread) != 0 || nread == 0) {
            return false;
        }
        total += (size_t)nread;
    }
    return true;
#else
    if (fseek(pack->file, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(dest, 1, size, pack->file) == size;
#endif
}

static TexturePageCacheEntry* TexturePackIndex_getDirectEntry(TexturePackIndex* pack, DataWin* dw, int page_id)
{
    if (!TexturePackIndex_ensureLoaded(pack, dw)) {
        return NULL;
    }

    if (!pack->fully_loaded || pack->direct_entries == NULL || page_id < 0 || (uint32_t)page_id >= pack->entry_count) {
        return NULL;
    }

    TexturePageCacheEntry* entry = &pack->direct_entries[page_id];
    if (!entry->loaded || entry->pixels_xdr == NULL) {
        return NULL;
    }

    return entry;
}

static bool loadTexturePageFromPack(DataWin* dw, int page_id, LoadedTexturePage* out_page)
{
    if (out_page == NULL) {
        return false;
    }

    if (!TexturePackIndex_ensureLoaded(&g_texturePack, dw)) {
        return false;
    }

    if (page_id < 0 || (uint32_t)page_id >= g_texturePack.entry_count || g_texturePack.entries == NULL) {
        return false;
    }

    const uint8_t* entry = g_texturePack.entries + ((size_t)page_id * (size_t)TXTR_PACK_ENTRY_SIZE);
    uint32_t width = TxtrPack_readU32LE(entry + 0);
    uint32_t height = TxtrPack_readU32LE(entry + 4);
    uint64_t dataOffset = TxtrPack_readU64LE(entry + 8);
    uint64_t dataSize = TxtrPack_readU64LE(entry + 16);
    uint32_t format = TxtrPack_readU32LE(entry + 32);
    uint32_t flags = TxtrPack_readU32LE(entry + 36);

    if ((flags & TXTR_PACK_ENTRY_FLAG_PRESENT) == 0u) {
        return false;
    }

    if ((format != TXTR_PACK_PIXEL_FORMAT_RGBA8 && format != TXTR_PACK_PIXEL_FORMAT_ARGB8) ||
        width == 0 ||
        height == 0) {
        fprintf(stderr, "TextureCache: invalid cooked texture metadata for page %d\n", page_id);
        return false;
    }

    size_t pixelCount = (size_t)width * (size_t)height;
    if (pixelCount == 0 || pixelCount > (SIZE_MAX / 4u) || dataSize != (uint64_t)(pixelCount * 4u)) {
        fprintf(stderr, "TextureCache: invalid cooked texture size for page %d\n", page_id);
        return false;
    }

    size_t allocSize = (size_t)dataSize;
#ifdef __PPU__
    allocSize = alignUpSize(allocSize, RSX_MAIN_MEMORY_MAP_ALIGNMENT);
    uint8_t* rgba = safeMemalign(RSX_MAIN_MEMORY_MAP_ALIGNMENT, allocSize);
    memset(rgba, 0, allocSize);
#else
    uint8_t* rgba = safeMalloc(allocSize);
#endif
    if (!TexturePackIndex_readBytesAt(&g_texturePack, dataOffset, rgba, (size_t)dataSize)) {
        fprintf(stderr, "TextureCache: failed to read cooked texture page %d from %s\n", page_id, g_texturePack.path);
        free(rgba);
        return false;
    }

    if (format == TXTR_PACK_PIXEL_FORMAT_RGBA8) {
        convertRgbaBytesToArgb(rgba, pixelCount);
    }
    out_page->pixels = rgba;
    out_page->size = allocSize;
    out_page->width = (int)width;
    out_page->height = (int)height;
    return true;
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

static void TexturePageCache_init(TexturePageCache *cache)
{
    memset(cache, 0, sizeof(*cache));
    TexturePackIndex_destroy(&g_texturePack);
}

static void TexturePageCache_destroy(TexturePageCache *cache)
{
    for (int i = 0; i < TEXTURE_CACHE_MAX_ENTRIES; i++) {
        TexturePageCacheEntry *entry = &cache->entries[i];

        if (entry->loaded && entry->pixels_xdr) {
            free(entry->pixels_xdr);
            entry->pixels_xdr = NULL;
        }
        entry->loaded = false;
        entry->size = 0;
        entry->width = 0;
        entry->height = 0;
        entry->page_id = -1;
        entry->last_used = 0;
    }

    cache->used_bytes = 0;
    cache->tick = 0;
    TexturePackIndex_destroy(&g_texturePack);
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
    TexturePageCacheEntry* direct = TexturePackIndex_getDirectEntry(&g_texturePack, dw, page_id);
    if (direct != NULL) {
        return direct;
    }

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

    return loadTexturePageFromPack(dw, page_id, out_page);
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

        TexturePageCache_preloadSprite(cache, dw, obj->spriteId, true, seen_pages);
    }

    free(seen_pages);
}

void SoftRenderer_TextureCacheInit(void)
{
    if (g_textureCacheInitCount > 0u)
    {
        g_textureCacheInitCount++;
        return;
    }

    TexturePageCache_init(&g_textureCache);
    g_textureCacheInitCount = 1u;
}

void SoftRenderer_TextureCacheWarmStart(DataWin *dw)
{
    (void)TexturePackIndex_ensureLoaded(&g_texturePack, dw);
}

void SoftRenderer_TextureCacheDestroy(void)
{
    if (g_textureCacheInitCount == 0u)
    {
        return;
    }

    g_textureCacheInitCount--;
    if (g_textureCacheInitCount > 0u)
    {
        return;
    }

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
