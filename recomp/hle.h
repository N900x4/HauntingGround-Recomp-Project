#ifndef HLE_H
#define HLE_H
#include <stdint.h>
void L(const char *fmt, ...);
void stop(const char *fmt, ...) __attribute__((noreturn));
uint8_t *gptr(uint32_t a);
int func_index(uint32_t a);
int func_containing(uint32_t a);
extern int bg_verbose;
uint64_t hle_hw_read(uint32_t pa, uint64_t stored, int size);
void hle_hw_write(uint32_t pa, uint64_t v, int size, uint8_t *hwreg);
uint64_t hle_gs_read(uint32_t pa, uint64_t stored, int size);
void hle_gs_write(uint32_t pa, uint64_t v, int size, uint8_t *gspriv);
void hle_dump(void);
const char *game_path(const char *name);
/* iop_pad_mc.c: DBCMAN (controller) and MCSERV (memory card) RPC servers */
int pad_rpc(uint32_t sid, uint32_t fn, const uint8_t *S, uint32_t ssize, uint32_t recv, uint32_t rsize);
int mc_rpc(uint32_t sid, uint32_t fn, const uint8_t *S, uint32_t ssize, uint32_t recv, uint32_t rsize);
void pad_vblank(uint64_t vblank);
void pad_set_input(uint16_t held, const uint8_t analog[4]);
/* gs.c / dmac.c: GS, GIF, DMA channels 1-2, VIF1 */
void gs_init(uint8_t *gspriv);
void gs_vblank(uint64_t vblank);
void gs_dump_stats(void);
void dma_run(int ch, uint8_t *hwreg);
void gif_fifo_write(uint32_t off, uint64_t v);
void dma_dump_stats(void);
void vu1_dump_stats(void);
/* host-time accounting per subsystem (seconds): 0 VU1, 1 GS drawing, 2 IOP+SPU2, 3 SPU2 mixing, 4 DMA/VIF */
extern double tprof[8];
double host_now(void);
/* iop_kernel.c: emulated IOP running the game's sound modules */
int iop_load_module(const char *path, const char *args, int arg_len);
int iop_rpc_find(uint32_t sid, uint32_t *sd, uint32_t *buf, uint32_t *cbuf);
int iop_rpc_call(uint32_t sd, uint32_t fno, uint32_t size, uint32_t recv, uint32_t rsize, const uint32_t endpkt[8]);
void iop_vblank(void);
int iop_serves_sid(uint32_t sid);
uint32_t iop_heap_alloc(uint32_t size);   /* IOP memory for sceSifAllocIopHeap */
int iop_heap_free(uint32_t addr);   /* sid belongs to a module running in the IOP emulator */
void iop_dump(void);
/* hle.c services for the IOP */
void hle_iop_rpc_end(const uint32_t endpkt[8]);
void hle_iop_send_cmd(uint32_t cid, const uint32_t *payload, int n);   /* IOP -> EE SIF command */
void iop_ee_rpc_end(const uint32_t *pkt);                                  /* EE -> IOP RPC END */
int hle_cd_read_iop(uint32_t lsn, uint32_t n, uint32_t iop_addr);
/* host_sdl.c: window + live input (no-ops when built without SDL or run with HG_WINDOW=0) */
int host_window_wanted(void);
void host_vblank(uint64_t vblank);
void host_audio_push(const int16_t *stereo, int frames);   /* 48 kHz stereo from the SPU2 */   /* $HG_GAME_DIR/name, default ../game/name */
void hle_run_main(uint32_t entry) __attribute__((noreturn));
#endif
