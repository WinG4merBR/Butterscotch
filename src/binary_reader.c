#include "binary_reader.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

#ifdef __PPU__
#include <lv2/sysfs.h>

static void lv2_read(s32 fd, void* dest, size_t bytes) {
    u8* ptr = (u8*)dest;
    size_t total = 0;
    while (total < bytes) {
        u64 nread __attribute__((aligned(8)));
        s32 ret = sysFsRead(fd, ptr + total, (u64)(bytes - total), &nread);
        if (ret != 0) {
            fprintf(stderr, "BinaryReader: sysFsRead error 0x%X at byte %lu/%lu\n",
                    ret, total, bytes);
            exit(1);
        }
        if (nread == 0) {
            fprintf(stderr, "BinaryReader: sysFsRead EOF at byte %lu/%lu\n",
                    total, bytes);
            exit(1);
        }
        total += (size_t)nread;
    }
}

static size_t lv2_tell(s32 fd) {
    u64 pos __attribute__((aligned(8)));
    sysFsLseek(fd, 0, SEEK_CUR, &pos);
    return (size_t)pos;
}

static void lv2_seek(s32 fd, size_t position) {
    u64 pos __attribute__((aligned(8)));
    int res = sysFsLseek(fd, (s64)position, SEEK_SET, &pos);
}

static void lv2_skip(s32 fd, size_t bytes) {
    u64 pos __attribute__((aligned(8)));
    sysFsLseek(fd, (s64)bytes, SEEK_CUR, &pos);
}
#endif

BinaryReader BinaryReader_create(FILE* file, size_t fileSize
#ifdef __PPU__
    , int lv2_fd
#endif
) {
    BinaryReader r = {0};
    r.file = file;
    r.fileSize = fileSize;
#ifdef __PPU__
    r.lv2_fd = lv2_fd;
#endif
    return r;
}

void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size) {
    reader->buffer = buffer;
    reader->bufferBase = baseOffset;
    reader->bufferSize = size;
    reader->bufferPos = 0;
}

void BinaryReader_clearBuffer(BinaryReader* reader) {
    reader->buffer = NULL;
    reader->bufferBase = 0;
    reader->bufferSize = 0;
    reader->bufferPos = 0;
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
    if (reader->buffer != NULL) {
        if (reader->bufferPos + bytes > reader->bufferSize) {
            size_t absPos = reader->bufferBase + reader->bufferPos;
            fprintf(stderr, "BinaryReader: buffer read error at 0x%zX\n", absPos);
            exit(1);
        }
        memcpy(dest, reader->buffer + reader->bufferPos, bytes);
        reader->bufferPos += bytes;
        return;
    }
#ifdef __PPU__
    lv2_read(reader->lv2_fd, dest, bytes);
#else
    size_t read = fread(dest, 1, bytes, reader->file);
    if (read != bytes) {
        long pos = ftell(reader->file) - (long)read;
        fprintf(stderr, "BinaryReader: read error at 0x%lX\n", pos);
        exit(1);
    }
#endif
}

uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t v; readCheck(reader, &v, 1); return v;
}

int16_t BinaryReader_readInt16(BinaryReader* reader) {
    int16_t v; readCheck(reader, &v, 2);
#ifdef __PPU__
    v = (int16_t)(((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8));
#endif
    return v;
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
    uint16_t v; readCheck(reader, &v, 2);
#ifdef __PPU__
    v = (uint16_t)(((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8));
#endif
    return v;
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
    int32_t v; readCheck(reader, &v, 4);
#ifdef __PPU__
    v = (int32_t)(((uint32_t)v >> 24) |
                  (((uint32_t)v >> 8) & 0x0000FF00) |
                  (((uint32_t)v << 8) & 0x00FF0000) |
                  ((uint32_t)v << 24));
#endif
    return v;
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
    uint32_t v; readCheck(reader, &v, 4);
#ifdef __PPU__
    v = ((v >> 24)) |
        ((v >> 8)  & 0x0000FF00) |
        ((v << 8)  & 0x00FF0000) |
        ((v << 24));
#endif
    return v;
}

float BinaryReader_readFloat32(BinaryReader* reader) {
    uint32_t v; readCheck(reader, &v, 4);
#ifdef __PPU__
    v = ((v >> 24)) |
        ((v >> 8)  & 0x0000FF00) |
        ((v << 8)  & 0x00FF0000) |
        ((v << 24));
#endif
    float f;
    memcpy(&f, &v, 4);
    return f;
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
    uint64_t v; readCheck(reader, &v, 8);
#ifdef __PPU__
    v = ((v >> 56)) |
        ((v >> 40) & 0x000000000000FF00ULL) |
        ((v >> 24) & 0x0000000000FF0000ULL) |
        ((v >> 8)  & 0x00000000FF000000ULL) |
        ((v << 8)  & 0x000000FF00000000ULL) |
        ((v << 24) & 0x0000FF0000000000ULL) |
        ((v << 40) & 0x00FF000000000000ULL) |
        ((v << 56));
#endif
    return v;
}

bool BinaryReader_readBool32(BinaryReader* reader) {
    return BinaryReader_readUint32(reader) != 0;
}

void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count) {
    readCheck(reader, dest, count);
}

uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count) {
    uint8_t* buf = safeMalloc(count);

    if (reader->buffer != NULL) {
        if (offset < reader->bufferBase || offset + count > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: readBytesAt out of buffer range\n");
            exit(1);
        }
        memcpy(buf, reader->buffer + (offset - reader->bufferBase), count);
        return buf;
    }

#ifdef __PPU__
    size_t savedPos = lv2_tell(reader->lv2_fd);
    lv2_seek(reader->lv2_fd, offset);
    lv2_read(reader->lv2_fd, buf, count);
    lv2_seek(reader->lv2_fd, savedPos);
#else
    long savedPos = ftell(reader->file);
    fseek(reader->file, (long)offset, SEEK_SET);
    readCheck(reader, buf, count);
    fseek(reader->file, savedPos, SEEK_SET);
#endif
    return buf;
}

void BinaryReader_skip(BinaryReader* reader, size_t bytes) {
    if (reader->buffer != NULL) {
        reader->bufferPos += bytes;
        return;
    }
#ifdef __PPU__
    lv2_skip(reader->lv2_fd, bytes);
#else
    fseek(reader->file, (long)bytes, SEEK_CUR);
#endif
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    if (reader->buffer != NULL) {
        if (position < reader->bufferBase || position > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: buffer seek out of range\n");
            exit(1);
        }
        reader->bufferPos = position - reader->bufferBase;
        return;
    }
    if (position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek out of bounds\n");
        exit(1);
    }
#ifdef __PPU__
    lv2_seek(reader->lv2_fd, position);
#else
    fseek(reader->file, (long)position, SEEK_SET);
#endif
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    if (reader->buffer != NULL) {
        return reader->bufferBase + reader->bufferPos;
    }
#ifdef __PPU__
    return lv2_tell(reader->lv2_fd);
#else
    return (size_t)ftell(reader->file);
#endif
}