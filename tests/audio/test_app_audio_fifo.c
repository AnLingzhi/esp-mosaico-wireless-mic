// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_audio_fifo.h"

static void test_wrap_preserves_order(void)
{
    uint8_t storage[8];
    app_audio_fifo_t fifo;
    app_audio_fifo_init(&fifo, storage, sizeof(storage));
    const uint8_t first[] = {1, 2, 3, 4, 5, 6};
    size_t dropped;
    assert(app_audio_fifo_write(&fifo, fifo.generation, first, sizeof(first), &dropped));
    assert(dropped == 0);
    uint8_t prefix[4];
    assert(app_audio_fifo_read(&fifo, prefix, sizeof(prefix)) == sizeof(prefix));
    const uint8_t second[] = {7, 8, 9, 10};
    assert(app_audio_fifo_write(&fifo, fifo.generation, second, sizeof(second), &dropped));
    uint8_t output[6];
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == sizeof(output));
    const uint8_t expected[] = {5, 6, 7, 8, 9, 10};
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_overflow_keeps_newest_bytes(void)
{
    uint8_t storage[5];
    app_audio_fifo_t fifo;
    app_audio_fifo_init(&fifo, storage, sizeof(storage));
    const uint8_t first[] = {1, 2, 3, 4};
    const uint8_t second[] = {5, 6, 7};
    size_t dropped;
    assert(app_audio_fifo_write(&fifo, fifo.generation, first, sizeof(first), &dropped));
    assert(app_audio_fifo_write(&fifo, fifo.generation, second, sizeof(second), &dropped));
    assert(dropped == 2);
    uint8_t output[5];
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == sizeof(output));
    const uint8_t expected[] = {3, 4, 5, 6, 7};
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_oversized_write_keeps_capacity_tail(void)
{
    uint8_t storage[4];
    app_audio_fifo_t fifo;
    app_audio_fifo_init(&fifo, storage, sizeof(storage));
    const uint8_t input[] = {1, 2, 3, 4, 5, 6};
    size_t dropped;
    assert(app_audio_fifo_write(&fifo, fifo.generation, input, sizeof(input), &dropped));
    assert(dropped == 2);
    uint8_t output[4];
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == sizeof(output));
    const uint8_t expected[] = {3, 4, 5, 6};
    assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_underflow_returns_available_bytes_only(void)
{
    uint8_t storage[4];
    app_audio_fifo_t fifo;
    app_audio_fifo_init(&fifo, storage, sizeof(storage));
    const uint8_t input[] = {9, 8};
    size_t dropped;
    assert(app_audio_fifo_write(&fifo, fifo.generation, input, sizeof(input), &dropped));
    uint8_t output[4] = {0};
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == 2);
    assert(output[0] == 9 && output[1] == 8);
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == 0);
}

static void test_reset_rejects_stale_generation(void)
{
    uint8_t storage[4];
    app_audio_fifo_t fifo;
    app_audio_fifo_init(&fifo, storage, sizeof(storage));
    const uint32_t stale_generation = fifo.generation;
    const uint8_t input[] = {1, 2};
    size_t dropped = 99;
    assert(app_audio_fifo_reset(&fifo) != stale_generation);
    assert(!app_audio_fifo_write(&fifo, stale_generation, input, sizeof(input), &dropped));
    assert(dropped == 0);
    uint8_t output[2];
    assert(app_audio_fifo_read(&fifo, output, sizeof(output)) == 0);
}

int main(void)
{
    test_wrap_preserves_order();
    test_overflow_keeps_newest_bytes();
    test_oversized_write_keeps_capacity_tail();
    test_underflow_returns_available_bytes_only();
    test_reset_rejects_stale_generation();
    return 0;
}
