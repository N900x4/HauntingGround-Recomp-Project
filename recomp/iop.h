#ifndef IOP_H
#define IOP_H
#include <stdint.h>

typedef struct {
    uint32_t r[32], hi, lo, pc, next_pc; int in_delay;
    uint32_t cop0[32];
    int yield;                 /* set by blocking HLE calls: leave iop_exec so the scheduler can switch */
} IopCpu;
extern IopCpu iop;
extern uint8_t iop_ram[2 << 20];
extern uint64_t iop_insns;

#define IOP_HLE_BASE 0x0FC10000u   /* HLE trap addresses (reachable by j from RAM): IOP_HLE_BASE + 8 * n */
#define IOP_HLE_END  0x0FC20000u
#define IOP_CLOCK 36864000.0

enum { IOP_IRQ_VBLANK = 0, IOP_IRQ_SPU = 9, IOP_IRQ_EVBLANK = 11, IOP_IRQ_DMA_SPU = 0x24, IOP_IRQ_DMA_SPU2 = 0x28 };

/* iop_cpu.c */
int iop_exec(long max);
uint8_t *iop_ptr(uint32_t a);
uint32_t iop_read(uint32_t a, int size);
void iop_write(uint32_t a, uint32_t v, int size);
void iop_raise_irq(int irq);
int iop_dma_busy(int core);

/* iop_kernel.c */
void iop_hle_trap(uint32_t pc);
void iop_syscall(uint32_t pc);
void iop_fault(const char *fmt, ...);
void iop_unmapped(uint32_t pa, uint32_t v, int size, int write);

/* spu2.c */
uint32_t spu2_read(uint32_t off, int size);
void spu2_write(uint32_t off, uint32_t v, int size);
int spu2_dma(int core, uint8_t *mem, uint32_t halfwords, int to_spu);   /* 1 = done now, 0 = auto-DMA pending */
int spu2_adma_active(void);
void iop_dma_done(int core);   /* iop_cpu.c: finish a pending SPU2 DMA (clear busy, raise IRQ) */
void spu2_run(uint64_t samples);             /* advance the sound chip by n 48 kHz samples */
void spu2_dump_stats(void);
#endif
