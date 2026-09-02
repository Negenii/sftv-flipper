#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-8, polynomial 0x07, init 0x00, no reflection, no final xor. "123456789" -> 0xF4. */
uint8_t crc8_compute(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
