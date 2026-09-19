#ifndef MEM_H
#define MEM_H

#include <stdint.h>

// The 6301's whole address space. A14/A15 are wired, so every access decodes
// uniquely and no masking is needed anywhere.
#define MEM_SIZE     65536u

// Worst case is a program that transmits the entire address space.
#define CAPTURE_SIZE 65536u

extern uint8_t mem[MEM_SIZE];
extern uint8_t capture[CAPTURE_SIZE];

void     mem_init(void);        // $FF fill, then the built-in image at its load address
void     capture_reset(void);
void     capture_put(uint8_t b);
uint32_t capture_len(void);
uint32_t capture_lost(void);    // bytes dropped after the buffer filled

#endif
