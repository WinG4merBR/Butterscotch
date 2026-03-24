#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef __PPU__
#include <ppu-types.h>
#endif

typedef struct {
    FILE* file;
    size_t fileSize;
#ifdef __PPU__
    s32 lv2_fd;
#endif
    uint8_t* buffer;
    size_t bufferBase;
    size_t bufferSize;
    size_t bufferPos;
} BinaryReader;

BinaryReader BinaryReader_create(FILE* file, size_t fileSize
#ifdef __PPU__
    , s32 lv2_fd
#endif
);

void     BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size);
void     BinaryReader_clearBuffer(BinaryReader* reader);
uint8_t  BinaryReader_readUint8(BinaryReader* reader);
int16_t  BinaryReader_readInt16(BinaryReader* reader);
uint16_t BinaryReader_readUint16(BinaryReader* reader);
int32_t  BinaryReader_readInt32(BinaryReader* reader);
uint32_t BinaryReader_readUint32(BinaryReader* reader);
float    BinaryReader_readFloat32(BinaryReader* reader);
uint64_t BinaryReader_readUint64(BinaryReader* reader);
bool     BinaryReader_readBool32(BinaryReader* reader);
void     BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count);
uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count);
void     BinaryReader_skip(BinaryReader* reader, size_t bytes);
void     BinaryReader_seek(BinaryReader* reader, size_t position);
size_t   BinaryReader_getPosition(BinaryReader* reader);