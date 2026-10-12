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
void pad_set_input(uint16_t held, const uint8_t analog[4]);   /* $HG_GAME_DIR/name, default ../game/name */
void hle_run_main(uint32_t entry) __attribute__((noreturn));
#endif
