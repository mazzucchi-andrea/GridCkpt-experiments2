/*
 * SPDX-FileCopyrightText: 2026 Andrea Mazzucchi <andrea.mazzucchi@tutamail.com>
 * SPDX-FileCopyrightText: 2026 Francesco Quaglia
 * <francesco.quaglia@uniroma2.it>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <emmintrin.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "ckpt_setup.h"

#ifndef MOD
#define MOD 8
#endif

#ifndef ALLOCATOR_AREA_SIZE
#define ALLOCATOR_AREA_SIZE 0x100000
#endif

#ifndef WRITES
#define WRITES 950
#endif

#ifndef READS
#define READS 50
#endif

#ifndef CF
#define CF 0
#endif

#define LIMIT_8BIT ALLOCATOR_AREA_SIZE - 1
#define LIMIT_16BIT ALLOCATOR_AREA_SIZE - 2
#define LIMIT_32BIT ALLOCATOR_AREA_SIZE - 4
#define LIMIT_64BIT ALLOCATOR_AREA_SIZE - 8

void test_checkpoint(uint8_t* area, int8_t value_8bit, int16_t value_16bit,
                     int32_t value_32bit, int64_t value_64bit) {
    int offset;
    volatile uint8_t read_8bit_value;
    volatile uint16_t read_16bit_value;
    volatile uint32_t read_32bit_value;
    volatile uint64_t read_64bit_value;

    _set_ckpt(area);
    offset = 0;
    for (int i = 0; i < WRITES / 4; i++) {
        if (offset >= LIMIT_8BIT) {
            offset = 0;
        }
        *(int8_t*)(area + offset) = value_8bit;
        offset += 4;
    }
    for (int i = 0; i < WRITES / 4; i++) {
        if (offset >= LIMIT_16BIT) {
            offset = 0;
        }
        *(int16_t*)(area + offset) = value_16bit;
        offset += 4;
    }
    for (int i = 0; i < WRITES / 4; i++) {
        if (offset >= LIMIT_32BIT) {
            offset = 0;
        }
        *(int32_t*)(area + offset) = value_32bit;
        offset += 4;
    }
    for (int i = 0; i < WRITES / 4; i++) {
        if (offset >= LIMIT_64BIT) {
            offset = 0;
        }
        *(int64_t*)(area + offset) = value_64bit;
        offset += 4;
    }

    offset = 0;
    for (int i = 0; i < READS / 4; i++) {
        if (offset >= LIMIT_8BIT) {
            offset = 0;
        }
        read_8bit_value = *(int8_t*)(area + offset);
        offset += 4;
    }
    for (int i = 0; i < READS / 4; i++) {
        if (offset >= LIMIT_16BIT) {
            offset = 0;
        }
        read_16bit_value = *(int16_t*)(area + offset);
        offset += 4;
    }
    for (int i = 0; i < READS / 4; i++) {
        if (offset >= LIMIT_32BIT) {
            offset = 0;
        }
        read_32bit_value = *(int32_t*)(area + offset);
        offset += 4;
    }
    for (int i = 0; i < READS / 4; i++) {
        if (offset >= LIMIT_64BIT) {
            offset = 0;
        }
        read_64bit_value = *(int64_t*)(area + offset);
        offset += 4;
    }
}

void clean_cache(uint8_t* area) {
    int cache_line_size = __builtin_cpu_supports("sse2") ? 64 : 32;
    for (int i = 0; i < (2 * ALLOCATOR_AREA_SIZE + BITMAP_SIZE);
         i += (cache_line_size / 8)) {
        _mm_clflush((void*)(area + i));
    }
}

int main(void) {
    unsigned long base_addr;
    uint8_t* area;
    uint8_t value_8bit;
    uint16_t value_16bit;
    uint32_t value_32bit;
    uint64_t value_64bit;
    size_t size;
    FILE* file;

    _tls_setup();

    srand(42);
    value_8bit = rand() % UINT8_MAX;
    value_16bit = rand() % UINT16_MAX;
    value_32bit = rand() % UINT32_MAX;
    value_64bit = rand() % UINT64_MAX;

    base_addr = 8UL * 1024UL * ALLOCATOR_AREA_SIZE;
    size = 2 * ALLOCATOR_AREA_SIZE + BITMAP_SIZE;
    area = (uint8_t*)mmap((void*)base_addr, size, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (area == MAP_FAILED) {
        perror("mmap failed");
        return errno;
    }

#if CF == 1
    clean_cache(area);
#endif
    test_checkpoint(area, value_8bit, value_16bit, value_32bit, value_64bit);
#if CF == 1
    clean_cache(area);
#endif
    _restore_area(area);

    return EXIT_SUCCESS;
}
