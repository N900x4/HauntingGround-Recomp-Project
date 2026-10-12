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
void hle_run_main(uint32_t entry) __attribute__((noreturn));
#endif
