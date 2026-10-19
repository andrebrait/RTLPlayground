#ifndef _RTL837X_FLASH_H_
#define _RTL837X_FLASH_H_

#include "platform.h"

// SPI FLASH MEMORY PAGE SIZE.
#define FLASH_PAGE_SIZE 0x100
// SPI FLASH MEMORY SECTOR SIZE = ERASE SIZE.
#define FLASH_SECTOR_SIZE 0x1000

#if (FLASH_SECTOR_SIZE % FLASH_PAGE_SIZE) != 0
#error "FLASH_SECTOR_SIZE must be a multiple of FLASH_PAGE_SIZE"
#endif


void flash_init(uint8_t enable_dio);
void flash_dump(uint8_t len);
void flash_sector_erase(void);
void flash_write_bytes(__xdata uint8_t *ptr);

#endif

