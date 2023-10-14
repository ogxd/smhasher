#ifndef GXHASH_H
#define GXHASH_H

#include <stdint.h>

#if defined (__cplusplus)
extern "C" {
#endif
// Function prototype for gxhash
uint64_t gxhash(const uint8_t* input, int len, uint32_t seed);
#if defined (__cplusplus)
}
#endif

#endif // GXHASH_H
