// SPDX-License-Identifier: Apache-2.0

#include "app_audio_fifo.h"

#include <string.h>

void app_audio_fifo_init(app_audio_fifo_t *fifo, uint8_t *storage, size_t capacity)
{
    *fifo = (app_audio_fifo_t) {
        .storage = storage,
        .capacity = capacity,
        .generation = 1,
    };
}

uint32_t app_audio_fifo_reset(app_audio_fifo_t *fifo)
{
    fifo->read_offset = 0;
    fifo->size = 0;
    fifo->generation++;
    if (fifo->generation == 0) {
        fifo->generation = 1;
    }
    return fifo->generation;
}

static void copy_into_fifo(app_audio_fifo_t *fifo, const uint8_t *src, size_t bytes)
{
    const size_t write_offset = (fifo->read_offset + fifo->size) % fifo->capacity;
    size_t first = fifo->capacity - write_offset;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(fifo->storage + write_offset, src, first);
    memcpy(fifo->storage, src + first, bytes - first);
    fifo->size += bytes;
}

bool app_audio_fifo_write(app_audio_fifo_t *fifo, uint32_t generation,
                          const uint8_t *src, size_t bytes, size_t *dropped_bytes)
{
    *dropped_bytes = 0;
    if (generation != fifo->generation) {
        return false;
    }
    if (bytes >= fifo->capacity) {
        *dropped_bytes = fifo->size + bytes - fifo->capacity;
        src += bytes - fifo->capacity;
        bytes = fifo->capacity;
        fifo->read_offset = 0;
        fifo->size = 0;
    } else if (bytes > fifo->capacity - fifo->size) {
        const size_t discard = bytes - (fifo->capacity - fifo->size);
        fifo->read_offset = (fifo->read_offset + discard) % fifo->capacity;
        fifo->size -= discard;
        *dropped_bytes = discard;
    }
    copy_into_fifo(fifo, src, bytes);
    return true;
}

size_t app_audio_fifo_read(app_audio_fifo_t *fifo, uint8_t *dst, size_t bytes)
{
    if (bytes > fifo->size) {
        bytes = fifo->size;
    }
    size_t first = fifo->capacity - fifo->read_offset;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(dst, fifo->storage + fifo->read_offset, first);
    memcpy(dst + first, fifo->storage, bytes - first);
    fifo->read_offset = (fifo->read_offset + bytes) % fifo->capacity;
    fifo->size -= bytes;
    return bytes;
}
