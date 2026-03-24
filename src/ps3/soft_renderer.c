#include "soft_renderer.h"
#include "data_win.h"
#include "utils.h"
#include "text_utils.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DEBUG_TILE_RENDER 0
#define TILE_TPAG_HOTFIX_DIRECT_OFFSET 0

// ===[ Renderer internal state ]===

typedef struct {
    Renderer base; // MUST be first

    uint32_t* fb;
    int fbWidth;
    int fbHeight;
    int gameWidth;
    int gameHeight;

    // Scale: fb pixels per game pixel
    float scaleX;
    float scaleY;

    // View/port offset in fb pixels:
    // fbX = gameX * scaleX + offsetX
    // where offsetX = portX - viewX * scaleX
    float offsetX;
    float offsetY;

    // Base letterbox offset/scale for mapping game space to framebuffer space.
    float frameScaleX;
    float frameScaleY;
    float frameOffsetX;
    float frameOffsetY;

    // View origin (subtracted before scaling, same as PS2 gs_renderer_flat)
    int32_t viewX;
    int32_t viewY;

    // Active clip rectangle in framebuffer pixels [x0, x1) x [y0, y1)
    int32_t clipX0;
    int32_t clipY0;
    int32_t clipX1;
    int32_t clipY1;

    uint64_t frameCounter;
    uint32_t frameSpriteCount;
    uint32_t framePixelWrites;
    uint32_t frameDrawSpriteCalls;
    uint32_t frameDrawSpritePartCalls;
    uint32_t frameDrawTextCalls;
    uint32_t frameRectCalls;
    uint32_t frameLineCalls;
    uint32_t frameBeginViewCalls;
    uint32_t frameClipRejects;
    uint32_t frameLowAlphaBlits;
    uint32_t frameBlackTintBlits;
    uint32_t frameNearBlackOutPixels;
    uint32_t frameOpaqueBlits;
    uint32_t frameDarkTintSpriteCalls;
    uint32_t frameDarkTintSpritePartCalls;
    uint32_t frameTileLegacyModeCalls;
    uint32_t frameTileRawModeCalls;
    uint32_t frameTileAbsoluteModeCalls;
    uint32_t frameTilePageRemapCalls;

    int32_t lastViewX;
    int32_t lastViewY;
    int32_t lastViewW;
    int32_t lastViewH;
    int32_t lastPortX;
    int32_t lastPortY;
    int32_t lastPortW;
    int32_t lastPortH;

    int textureCount;
    uint32_t* framePageSampleCounts;
    uint32_t* framePageNearBlackSamples;
} SoftRendererInternal;

// ===[ Pixel helpers ]===

static inline uint32_t rgbaToArgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void bgrToRgbF(uint32_t bgr, float* r, float* g, float* b) {
    *r = ((bgr)       & 0xFF) / 255.0f;
    *g = ((bgr >> 8)  & 0xFF) / 255.0f;
    *b = ((bgr >> 16) & 0xFF) / 255.0f;
}

static inline uint32_t blendPixel(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0)   return dst;
    if (sa == 255) return src;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t inv = 255 - sa;
    uint32_t or_ = (((src >> 16) & 0xFF) * sa + ((dst >> 16) & 0xFF) * inv) / 255;
    uint32_t og  = (((src >>  8) & 0xFF) * sa + ((dst >>  8) & 0xFF) * inv) / 255;
    uint32_t ob  = (((src)       & 0xFF) * sa + ((dst)       & 0xFF) * inv) / 255;
    uint32_t oa  = sa + (da * inv) / 255;
    return ((oa & 0xFF) << 24) | ((or_ & 0xFF) << 16) | ((og & 0xFF) << 8) | (ob & 0xFF);
}

static inline void fbPut(SoftRendererInternal* ri, int fx, int fy, uint32_t px) {
    if ((unsigned)fx >= (unsigned)ri->fbWidth || (unsigned)fy >= (unsigned)ri->fbHeight) {
        return;
    }

    if (ri->clipX0 != 0 || ri->clipY0 != 0 || ri->clipX1 != ri->fbWidth || ri->clipY1 != ri->fbHeight) {
        if (fx < ri->clipX0 || fx >= ri->clipX1 || fy < ri->clipY0 || fy >= ri->clipY1) {
            return;
        }
    }

    uint32_t* dst = &ri->fb[fy * ri->fbWidth + fx];
    if ((px >> 24) == 0xFF) {
        *dst = px;
        return;
    }

    *dst = blendPixel(*dst, px);
}

static int resolvePageId(SoftRendererInternal* ri, int16_t texturePageId) {
    int pageId = (int)texturePageId;
    if (pageId >= 0 && pageId < ri->textureCount) return pageId;
    return -1;
}

// ===[ Texture loading ]===

static void loadTextures(SoftRendererInternal* ri, DataWin* dataWin) {
    ri->textureCount = (int)dataWin->txtr.count;
    ri->framePageSampleCounts = nullptr;
    ri->framePageNearBlackSamples = nullptr;
    SoftRenderer_TextureCacheInit();
    fprintf(stderr, "SoftRenderer: initialized %d texture pages (cache-backed lazy decode)\n", ri->textureCount);
}

static TexturePageCacheEntry *getTexturePage(SoftRendererInternal *ri, int pageId) {
    if (!ri) return NULL;
    if (pageId < 0 || (uint32_t)pageId >= (uint32_t)ri->textureCount) return NULL;
    return SoftRenderer_TextureCacheGetPage(ri->base.dataWin, pageId);
}

// ===[ Core sprite blit ]===

static void blitSprite(SoftRendererInternal* ri,
                       int pageId,
                       int srcX, int srcY, int srcW, int srcH,
                       float dstXf, float dstYf,
                       float xscale, float yscale,
                       float angleDeg,
                       float tintR, float tintG, float tintB,
                       float alpha)
{
    if (!ri->fb || pageId < 0 || pageId >= ri->textureCount) return;
    TexturePageCacheEntry* tex = getTexturePage(ri, pageId);
    if (alpha <= 0.0f || srcW <= 0 || srcH <= 0) return;
    if (!tex || !tex->loaded || !tex->pixels_xdr) return;

    uint32_t* pixels = (uint32_t*)tex->pixels_xdr;

    float fbDstX = (dstXf - (float)ri->viewX) * ri->scaleX + ri->offsetX;
    float fbDstY = (dstYf - (float)ri->viewY) * ri->scaleY + ri->offsetY;
    float fbDstW = (float)srcW * xscale * ri->scaleX;
    float fbDstH = (float)srcH * yscale * ri->scaleY;
    if (fabsf(fbDstW) < 0.0001f || fabsf(fbDstH) < 0.0001f) return;

    const bool identityTint = tintR >= 0.999f && tintG >= 0.999f && tintB >= 0.999f;
    const bool opaqueAlpha = alpha >= 0.999f;

    if (fabsf(angleDeg) < 0.01f) {
        float xStart = fbDstX;
        float yStart = fbDstY;
        float xEnd = fbDstX + fbDstW;
        float yEnd = fbDstY + fbDstH;
        if (xStart > xEnd) { float t = xStart; xStart = xEnd; xEnd = t; }
        if (yStart > yEnd) { float t = yStart; yStart = yEnd; yEnd = t; }

        int x0 = (int)xStart;
        int y0 = (int)yStart;
        int x1 = (int)(xEnd + 0.5f);
        int y1 = (int)(yEnd + 0.5f);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > ri->fbWidth)  x1 = ri->fbWidth;
        if (y1 > ri->fbHeight) y1 = ri->fbHeight;
        if (x0 >= x1 || y0 >= y1) return;

        float srcPerFbX = (float)srcW / fbDstW;
        float srcPerFbY = (float)srcH / fbDstH;

        for (int fy = y0; fy < y1; fy++) {
            int ty = srcY + (int)((((float)fy - fbDstY) + 0.5f) * srcPerFbY);
            if (ty < 0) ty = 0;
            if (ty >= tex->height) ty = tex->height - 1;

            for (int fx = x0; fx < x1; fx++) {
                int tx = srcX + (int)((((float)fx - fbDstX) + 0.5f) * srcPerFbX);
                if (tx < 0) tx = 0;
                if (tx >= tex->width) tx = tex->width - 1;

                uint32_t texPx = pixels[ty * tex->width + tx];
                uint8_t ta = (texPx >> 24) & 0xFF;
                if (ta == 0) continue;

                if (identityTint && opaqueAlpha) {
                    fbPut(ri, fx, fy, texPx);
                    continue;
                }

                uint8_t fa = opaqueAlpha ? ta : (uint8_t)((float)ta * alpha);
                if (identityTint) {
                    fbPut(ri, fx, fy, (texPx & 0x00FFFFFFu) | ((uint32_t)fa << 24));
                    continue;
                }

                uint8_t pr = (uint8_t)(((texPx >> 16) & 0xFF) * tintR);
                uint8_t pg = (uint8_t)(((texPx >>  8) & 0xFF) * tintG);
                uint8_t pb = (uint8_t)(((texPx)       & 0xFF) * tintB);
                fbPut(ri, fx, fy, rgbaToArgb(pr, pg, pb, fa));
            }
        }
    } else {
        float angle = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(angle), sinA = sinf(angle);
        float cx = fbDstX + fbDstW * 0.5f;
        float cy = fbDstY + fbDstH * 0.5f;
        float drawW = fabsf(fbDstW);
        float drawH = fabsf(fbDstH);
        float hw = drawW * 0.5f, hh = drawH * 0.5f;
        float invDrawW = (drawW > 0.0f) ? (1.0f / drawW) : 0.0f;
        float invDrawH = (drawH > 0.0f) ? (1.0f / drawH) : 0.0f;

        float c[4][2] = {{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}};
        float mn[2] = {1e9f,1e9f}, mx[2] = {-1e9f,-1e9f};
        for (int i = 0; i < 4; i++) {
            float rx = c[i][0]*cosA - c[i][1]*sinA + cx;
            float ry = c[i][0]*sinA + c[i][1]*cosA + cy;
            if (rx < mn[0]) mn[0] = rx;
            if (rx > mx[0]) mx[0] = rx;
            if (ry < mn[1]) mn[1] = ry;
            if (ry > mx[1]) mx[1] = ry;
        }

        int x0 = (int)mn[0]; if (x0 < 0) x0 = 0;
        int y0 = (int)mn[1]; if (y0 < 0) y0 = 0;
        int x1 = (int)(mx[0] + 1); if (x1 > ri->fbWidth) x1 = ri->fbWidth;
        int y1 = (int)(mx[1] + 1); if (y1 > ri->fbHeight) y1 = ri->fbHeight;

        for (int fy = y0; fy < y1; fy++) {
            for (int fx = x0; fx < x1; fx++) {
                float dx = (float)fx - cx, dy = (float)fy - cy;
                float lx = dx*cosA + dy*sinA, ly = -dx*sinA + dy*cosA;
                if (lx < -hw || lx > hw || ly < -hh || ly > hh) continue;

                float u = (lx + hw) * invDrawW;
                float v = (ly + hh) * invDrawH;
                if (fbDstW < 0.0f) u = 1.0f - u;
                if (fbDstH < 0.0f) v = 1.0f - v;

                int tx = srcX + (int)(u * (float)srcW);
                int ty = srcY + (int)(v * (float)srcH);
                if (tx < 0) tx = 0;
                if (tx >= tex->width) tx = tex->width - 1;
                if (ty < 0) ty = 0;
                if (ty >= tex->height) ty = tex->height - 1;

                uint32_t texPx = pixels[ty * tex->width + tx];
                uint8_t ta = (texPx >> 24) & 0xFF;
                if (ta == 0) continue;

                if (identityTint && opaqueAlpha) {
                    fbPut(ri, fx, fy, texPx);
                    continue;
                }

                uint8_t fa = opaqueAlpha ? ta : (uint8_t)((float)ta * alpha);
                if (identityTint) {
                    fbPut(ri, fx, fy, (texPx & 0x00FFFFFFu) | ((uint32_t)fa << 24));
                    continue;
                }

                uint8_t pr = (uint8_t)(((texPx >> 16) & 0xFF) * tintR);
                uint8_t pg = (uint8_t)(((texPx >>  8) & 0xFF) * tintG);
                uint8_t pb = (uint8_t)(((texPx)       & 0xFF) * tintB);
                fbPut(ri, fx, fy, rgbaToArgb(pr, pg, pb, fa));
            }
        }
    }
}

// ===[ Vtable ]===

static void soft_init(Renderer* r, DataWin* dataWin) {
    loadTextures((SoftRendererInternal*)r, dataWin);
}

static void soft_destroy(Renderer* r) {
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    free(ri->framePageSampleCounts);
    free(ri->framePageNearBlackSamples);
    SoftRenderer_TextureCacheDestroy();
    free(ri);
}

static void soft_beginFrame(Renderer* r, int32_t gameW, int32_t gameH, int32_t wW, int32_t wH) {
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    (void)wW;
    (void)wH;
    ri->gameWidth = gameW;
    ri->gameHeight = gameH;
    ri->viewX = ri->viewY = 0;

    // Preserve aspect ratio to avoid crop on 16:9 framebuffers (e.g. 720p).
    if (gameW > 0 && gameH > 0) {
        float sx = (float)ri->fbWidth / (float)gameW;
        float sy = (float)ri->fbHeight / (float)gameH;
        float s = (sx < sy) ? sx : sy;
        ri->scaleX = s;
        ri->scaleY = s;
    } else {
        ri->scaleX = 1.0f;
        ri->scaleY = 1.0f;
    }

    float renderedW = (float)gameW * ri->scaleX;
    float renderedH = (float)gameH * ri->scaleY;
    ri->offsetX = ((float)ri->fbWidth - renderedW) / 2.0f;
    ri->offsetY = ((float)ri->fbHeight - renderedH) / 2.0f;
    ri->frameScaleX = ri->scaleX;
    ri->frameScaleY = ri->scaleY;
    ri->frameOffsetX = ri->offsetX;
    ri->frameOffsetY = ri->offsetY;

    ri->clipX0 = 0;
    ri->clipY0 = 0;
    ri->clipX1 = ri->fbWidth;
    ri->clipY1 = ri->fbHeight;
}

static void soft_endFrame(Renderer* r) {
    (void)r;
}

static void soft_beginView(Renderer* r,
                           int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                           int32_t px, int32_t py, int32_t pw, int32_t ph,
                           float angle)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    (void)angle;
    ri->lastViewX = vx;
    ri->lastViewY = vy;
    ri->lastViewW = vw;
    ri->lastViewH = vh;
    ri->lastPortX = px;
    ri->lastPortY = py;
    ri->lastPortW = pw;
    ri->lastPortH = ph;

    // Store view origin — subtracted in blitSprite/draw calls (same as PS2)
    ri->viewX = vx;
    ri->viewY = vy;

    // Ports are in game-space pixels. Convert to framebuffer-space first.
    float gameToFbX = ri->frameScaleX;
    float gameToFbY = ri->frameScaleY;
    float baseOffsetX = ri->frameOffsetX;
    float baseOffsetY = ri->frameOffsetY;

    // Guard against malformed room view/port data that can collapse rendering.
    if (vw <= 0) vw = (ri->gameWidth > 0) ? ri->gameWidth : 1;
    if (vh <= 0) vh = (ri->gameHeight > 0) ? ri->gameHeight : 1;
    if (pw <= 0) pw = (ri->gameWidth > 0) ? ri->gameWidth : vw;
    if (ph <= 0) ph = (ri->gameHeight > 0) ? ri->gameHeight : vh;

    // Map view rectangle into its port rectangle.
    ri->scaleX = gameToFbX * ((float)pw / (float)vw);
    ri->scaleY = gameToFbY * ((float)ph / (float)vh);
    ri->offsetX = baseOffsetX + (float)px * gameToFbX;
    ri->offsetY = baseOffsetY + (float)py * gameToFbY;

    // Clip drawing to the port bounds.
    ri->clipX0 = (int)(baseOffsetX + (float)px * gameToFbX);
    ri->clipY0 = (int)(baseOffsetY + (float)py * gameToFbY);
    ri->clipX1 = (int)(baseOffsetX + (float)(px + pw) * gameToFbX + 0.5f);
    ri->clipY1 = (int)(baseOffsetY + (float)(py + ph) * gameToFbY + 0.5f);
    if (ri->clipX0 < 0) ri->clipX0 = 0;
    if (ri->clipY0 < 0) ri->clipY0 = 0;
    if (ri->clipX1 > ri->fbWidth) ri->clipX1 = ri->fbWidth;
    if (ri->clipY1 > ri->fbHeight) ri->clipY1 = ri->fbHeight;

    // If the resulting clip is empty/offscreen, fallback to full frame mapping.
    if (ri->clipX1 <= ri->clipX0 || ri->clipY1 <= ri->clipY0) {
        ri->scaleX = ri->frameScaleX;
        ri->scaleY = ri->frameScaleY;
        ri->offsetX = ri->frameOffsetX;
        ri->offsetY = ri->frameOffsetY;
        ri->clipX0 = 0;
        ri->clipY0 = 0;
        ri->clipX1 = ri->fbWidth;
        ri->clipY1 = ri->fbHeight;
        fprintf(stderr,
                "SoftRenderer: beginView fallback (empty clip) vx=%d vy=%d vw=%d vh=%d px=%d py=%d pw=%d ph=%d\n",
                vx, vy, vw, vh, px, py, pw, ph);
    }
}

static void soft_endView(Renderer* r) {
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    ri->viewX = ri->viewY = 0;
    if (ri->gameWidth > 0 && ri->gameHeight > 0) {
        float sx = (float)ri->fbWidth / (float)ri->gameWidth;
        float sy = (float)ri->fbHeight / (float)ri->gameHeight;
        float s = (sx < sy) ? sx : sy;
        ri->scaleX = s;
        ri->scaleY = s;
    } else {
        ri->scaleX = 1.0f;
        ri->scaleY = 1.0f;
    }
    float renderedW = (float)ri->gameWidth * ri->scaleX;
    float renderedH = (float)ri->gameHeight * ri->scaleY;
    ri->offsetX = ((float)ri->fbWidth - renderedW) / 2.0f;
    ri->offsetY = ((float)ri->fbHeight - renderedH) / 2.0f;
    ri->frameScaleX = ri->scaleX;
    ri->frameScaleY = ri->scaleY;
    ri->frameOffsetX = ri->offsetX;
    ri->frameOffsetY = ri->offsetY;
    ri->clipX0 = 0;
    ri->clipY0 = 0;
    ri->clipX1 = ri->fbWidth;
    ri->clipY1 = ri->fbHeight;
}

static void soft_drawSprite(Renderer* r, int32_t tpagIndex, float x, float y,
                            float ox, float oy, float xs, float ys, float ang,
                            uint32_t color, float alpha)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    DataWin* dw = r->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int pageId = resolvePageId(ri, tpag->texturePageId);
    if (pageId < 0) return;
    float tr, tg, tb;
    bgrToRgbF(color, &tr, &tg, &tb);

    // Match GL semantics for trimmed atlas sprites:
    // local top-left is (targetX - originX, targetY - originY).
    float drawX = x + ((float)tpag->targetX - ox) * xs;
    float drawY = y + ((float)tpag->targetY - oy) * ys;

    blitSprite(ri, pageId,
               tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
               drawX, drawY, xs, ys, ang, tr, tg, tb, alpha);
}

static void soft_drawSpritePart(Renderer* r, int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                float x, float y, float xs, float ys, uint32_t color, float alpha)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    DataWin* dw = r->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int pageId = resolvePageId(ri, tpag->texturePageId);
    if (pageId < 0) return;

    int srcBaseX = tpag->sourceX + srcOffX;
    int srcBaseY = tpag->sourceY + srcOffY;

    float tr, tg, tb;
    bgrToRgbF(color, &tr, &tg, &tb);

    blitSprite(ri, pageId,
               srcBaseX, srcBaseY, srcW, srcH,
               x, y, xs, ys, 0.0f, tr, tg, tb, alpha);
}

static void soft_drawTile(Renderer* r, RoomTile* tile, float offsetX, float offsetY) {
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    DataWin* dw = r->dataWin;

    if (!tile) {
        fprintf(stderr, "[Tile] skipped: tile == NULL\n");
        return;
    }

    if (tile->backgroundDefinition < 0 || (uint32_t)tile->backgroundDefinition >= dw->bgnd.count) {
        fprintf(stderr,
            "[Tile] skipped: invalid backgroundDefinition=%d bgnd.count=%u\n",
            tile->backgroundDefinition, dw->bgnd.count);
        return;
    }

    Background* bg = &dw->bgnd.backgrounds[tile->backgroundDefinition];
    int32_t tpagIndex = DataWin_resolveTPAG(dw, bg->textureOffset);

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) {
        fprintf(stderr,
            "[Tile] skipped: invalid tpagIndex=%d tpag.count=%u textureOffset=%d\n",
            tpagIndex, dw->tpag.count, bg->textureOffset);
        return;
    }

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int pageId = resolvePageId(ri, tpag->texturePageId);

    if (pageId < 0) {
        fprintf(stderr, "[Tile] skipped: invalid pageId=%d\n", pageId);
        return;
    }

    int srcX = tile->sourceX;
    int srcY = tile->sourceY;
    int srcW = (int)tile->width;
    int srcH = (int)tile->height;

    if (srcW <= 0 || srcH <= 0) {
        fprintf(stderr, "[Tile] skipped: invalid src size %dx%d\n", srcW, srcH);
        return;
    }

    float drawX = (float)tile->x + offsetX;
    float drawY = (float)tile->y + offsetY;

    int contentLeft = (int)tpag->targetX;
    int contentTop = (int)tpag->targetY;
    int contentRight = (int)tpag->targetX + (int)tpag->sourceWidth;
    int contentBottom = (int)tpag->targetY + (int)tpag->sourceHeight;

    if (contentLeft > srcX) {
        int clip = contentLeft - srcX;
        drawX += (float)clip * tile->scaleX;
        srcW -= clip;
        srcX = contentLeft;
    }
    if (contentTop > srcY) {
        int clip = contentTop - srcY;
        drawY += (float)clip * tile->scaleY;
        srcH -= clip;
        srcY = contentTop;
    }
    if (srcX + srcW > contentRight) srcW = contentRight - srcX;
    if (srcY + srcH > contentBottom) srcH = contentBottom - srcY;

    if (srcW <= 0 || srcH <= 0) {
        fprintf(stderr, "[Tile] skipped: fully clipped away\n");
        return;
    }

    int legacyOffX = srcX - (int)tpag->targetX;
    int legacyOffY = srcY - (int)tpag->targetY;
    int rawOffX = srcX;
    int rawOffY = srcY;

    int legacySrcX = (int)tpag->sourceX + legacyOffX;
    int legacySrcY = (int)tpag->sourceY + legacyOffY;
    int rawSrcX = (int)tpag->sourceX + rawOffX;
    int rawSrcY = (int)tpag->sourceY + rawOffY;
    int absSrcX = srcX;
    int absSrcY = srcY;

    uint8_t alphaByte = (tile->color >> 24) & 0xFF;
    float alpha = (alphaByte == 0) ? 1.0f : (float)alphaByte / 255.0f;

    uint32_t bgr = tile->color & 0x00FFFFFFu;

    float tr, tg, tb;
    bgrToRgbF(bgr, &tr, &tg, &tb);

    int debugMode = 0;

    int finalSrcX = rawSrcX;
    int finalSrcY = rawSrcY;

    if (debugMode == 0) {
        finalSrcX = legacySrcX;
        finalSrcY = legacySrcY;
    } else if (debugMode == 1) {
        finalSrcX = rawSrcX;
        finalSrcY = rawSrcY;
    } else {
        finalSrcX = absSrcX;
        finalSrcY = absSrcY;
    }

    blitSprite(ri,
               pageId,
               finalSrcX,
               finalSrcY,
               srcW,
               srcH,
               drawX,
               drawY,
               tile->scaleX,
               tile->scaleY,
               0.0f,
               tr,
               tg,
               tb,
               alpha);
}

static void soft_drawRectangle(Renderer* r, float x1, float y1, float x2, float y2,
                               uint32_t color, float alpha, bool outline)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    if (!ri->fb) return;
    float tr, tg, tb;
    bgrToRgbF(color, &tr, &tg, &tb);
    uint32_t px = rgbaToArgb((uint8_t)(tr*255),(uint8_t)(tg*255),(uint8_t)(tb*255),(uint8_t)(alpha*255));
    int ix0 = (int)((x1-(float)ri->viewX)*ri->scaleX+ri->offsetX);
    int iy0 = (int)((y1-(float)ri->viewY)*ri->scaleY+ri->offsetY);
    int ix1 = (int)((x2-(float)ri->viewX)*ri->scaleX+ri->offsetX);
    int iy1 = (int)((y2-(float)ri->viewY)*ri->scaleY+ri->offsetY);
    if (ix0<0) ix0=0;
    if (iy0<0) iy0=0;
    if (ix1>ri->fbWidth) ix1=ri->fbWidth;
    if (iy1>ri->fbHeight) iy1=ri->fbHeight;
    if (outline) {
        for (int fx=ix0;fx<ix1;fx++){fbPut(ri,fx,iy0,px);fbPut(ri,fx,iy1-1,px);}
        for (int fy=iy0;fy<iy1;fy++){fbPut(ri,ix0,fy,px);fbPut(ri,ix1-1,fy,px);}
    } else {
        for (int fy=iy0;fy<iy1;fy++) for (int fx=ix0;fx<ix1;fx++) fbPut(ri,fx,fy,px);
    }
}

static void soft_drawLine(Renderer* r, float x1, float y1, float x2, float y2,
                          float width, uint32_t color, float alpha)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    if (!ri->fb) return;
    (void)width;
    float tr, tg, tb;
    bgrToRgbF(color, &tr, &tg, &tb);
    uint32_t px = rgbaToArgb((uint8_t)(tr*255),(uint8_t)(tg*255),(uint8_t)(tb*255),(uint8_t)(alpha*255));
    int fx0 = (int)((x1-(float)ri->viewX)*ri->scaleX+ri->offsetX);
    int fy0 = (int)((y1-(float)ri->viewY)*ri->scaleY+ri->offsetY);
    int fx1 = (int)((x2-(float)ri->viewX)*ri->scaleX+ri->offsetX);
    int fy1 = (int)((y2-(float)ri->viewY)*ri->scaleY+ri->offsetY);
    int dx = abs(fx1-fx0), sx = fx0<fx1?1:-1, dy = abs(fy1-fy0), sy = fy0<fy1?1:-1;
    int err = (dx>dy?dx:-dy)/2;
    while(1){
        fbPut(ri,fx0,fy0,px);
        if(fx0==fx1&&fy0==fy1)break;
        int e2=err;
        if(e2>-dx){err-=dy;fx0+=sx;}
        if(e2<dy){err+=dx;fy0+=sy;}
    }
}

static void soft_drawLineColor(Renderer* r, float x1, float y1, float x2, float y2,
                               float w, uint32_t c1, uint32_t c2, float alpha)
{
    (void)c2;
    soft_drawLine(r,x1,y1,x2,y2,w,c1,alpha);
}

static void soft_drawText(Renderer* r, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg)
{
    SoftRendererInternal* ri = (SoftRendererInternal*)r;
    DataWin* dw = r->dataWin;
    if (!ri->fb || !text || *text == '\0') return;
    (void)angleDeg;

    int fontIndex = r->drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;
    Font* font = &dw->font.fonts[fontIndex];

    int tpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* ftpag = &dw->tpag.items[tpagIndex];
    int pageId = resolvePageId(ri, ftpag->texturePageId);
    if (pageId < 0) return;
    uint32_t textColor = r->drawColor;
    float tr, tg, tb;
    bgrToRgbF(textColor, &tr, &tg, &tb);
    float alpha = r->drawAlpha;

    // Match PS2/GL behavior:
    // 1) preprocess GML '#'/ '\#' => line breaks / literal '#'
    // 2) apply drawHalign/drawValign offsets
    // 3) support UTF-8 glyphs + kerning
    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);
    if (0 >= textLen) { free(processed); return; }

    int32_t lineCount = TextUtils_countLines(processed, textLen);
    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0.0f;
    if (r->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (r->drawValign == 2) valignOffset = -totalHeight;

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
            lineEnd++;
        }

        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (r->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (r->drawHalign == 2) halignOffset = -lineWidth;
        float cursorX = halignOffset;

        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == NULL) continue;

            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += (float) glyph->shift;
                continue;
            }

            float glyphX = x + (cursorX + (float) glyph->offset) * xscale * font->scaleX;
            float glyphY = y + cursorY * yscale * font->scaleY;

            blitSprite(ri, pageId,
                       ftpag->sourceX + glyph->sourceX,
                       ftpag->sourceY + glyph->sourceY,
                       glyph->sourceWidth, glyph->sourceHeight,
                       glyphX, glyphY,
                       xscale * font->scaleX, yscale * font->scaleY,
                       0.0f, tr, tg, tb, alpha);

            cursorX += (float) glyph->shift;

            // Apply kerning using the next UTF-8 codepoint.
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        } else {
            break;
        }
    }

    free(processed);
}

static void soft_flush(Renderer* r) { (void)r; }

static RendererVtable g_softVtable = {
    .init = soft_init,
    .destroy = soft_destroy,
    .beginFrame = soft_beginFrame,
    .endFrame = soft_endFrame,
    .beginView = soft_beginView,
    .endView = soft_endView,
    .drawSprite = soft_drawSprite,
    .drawSpritePart = soft_drawSpritePart,
    .drawRectangle = soft_drawRectangle,
    .drawLine = soft_drawLine,
    .drawLineColor = soft_drawLineColor,
    .drawText = soft_drawText,
    .flush = soft_flush,
    .createSpriteFromSurface = NULL,
    .deleteSprite = NULL,
    .drawTile = soft_drawTile,
};

Renderer* SoftRenderer_create(DataWin* dataWin) {
    SoftRendererInternal* ri = (SoftRendererInternal*)safeCalloc(1, sizeof(SoftRendererInternal));
    ri->base.vtable = &g_softVtable;
    ri->base.dataWin = dataWin;
    ri->base.drawColor = 0xFFFFFF;
    ri->base.drawAlpha = 1.0f;
    ri->base.drawFont = -1;
    soft_init(&ri->base, dataWin);

    return &ri->base;
}

void SoftRenderer_preloadRoom(Renderer* renderer, Room* room) {
    SoftRendererInternal* ri = (SoftRendererInternal*)renderer;
    if (!ri || !room) return;
    SoftRenderer_TextureCachePreloadRoom(ri->base.dataWin, room);
}

void SoftRenderer_setBuffer(Renderer* renderer, uint32_t* fb, int fbW, int fbH, int gameW, int gameH) {
    SoftRendererInternal* ri = (SoftRendererInternal*)renderer;
    ri->fb = fb;
    ri->fbWidth = fbW;
    ri->fbHeight = fbH;
    ri->gameWidth = gameW;
    ri->gameHeight = gameH;
    ri->viewX = ri->viewY = 0;
    if (gameW > 0 && gameH > 0) {
        float sx = (float)fbW / (float)gameW;
        float sy = (float)fbH / (float)gameH;
        float s = (sx < sy) ? sx : sy;
        ri->scaleX = s;
        ri->scaleY = s;
    } else {
        ri->scaleX = 1.0f;
        ri->scaleY = 1.0f;
    }
    float renderedW = (float)gameW * ri->scaleX;
    float renderedH = (float)gameH * ri->scaleY;
    ri->offsetX = ((float)fbW - renderedW) / 2.0f;
    ri->offsetY = ((float)fbH - renderedH) / 2.0f;
    ri->frameScaleX = ri->scaleX;
    ri->frameScaleY = ri->scaleY;
    ri->frameOffsetX = ri->offsetX;
    ri->frameOffsetY = ri->offsetY;
    ri->clipX0 = 0;
    ri->clipY0 = 0;
    ri->clipX1 = fbW;
    ri->clipY1 = fbH;
}

void SoftRenderer_destroy(Renderer* renderer) {
    soft_destroy(renderer);
}
