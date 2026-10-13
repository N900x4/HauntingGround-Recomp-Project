#ifndef VU1_STATE_H
#define VU1_STATE_H
#include <stdint.h>
/* VU1 register file, for loading/storing the interpreters' state (lockstep checking) */
typedef struct {
    uint32_t vf[32][4], acc[4]; uint16_t vi[16];
    float q, p, i, q_pend, p_pend; int q_wait, p_wait;
    uint32_t r, mac, status, clip;
} Vu1State;
#endif
