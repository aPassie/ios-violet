#pragma once
#include <stdint.h>
int emmc_init(void);
int emmc_fast(void);
int emmc_read(uint32_t lba, uint32_t *buf);	/* one 512-byte sector, buf 4-byte aligned */
extern uint32_t emmc_ext_csd[128];
