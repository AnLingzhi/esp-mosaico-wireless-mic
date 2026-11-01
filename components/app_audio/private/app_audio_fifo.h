// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *storage;
    size_t capacity;
    size_t read_offset;
    size_t size;
    uint32_t generation;
} app_audio_fifo_t;

void app_audio_fifo_init(app_audio_fifo_t *fifo, uint8_t *storage, size_t capacity);
uint32_t app_audio_fifo_reset(app_audio_fifo_t *fifo);
bool app_audio_fifo_write(app_audio_fifo_t *fifo, uint32_t generation,
                          const uint8_t *src, size_t bytes, size_t *dropped_bytes);
size_t app_audio_fifo_read(app_audio_fifo_t *fifo, uint8_t *dst, size_t bytes);
