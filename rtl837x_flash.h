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

// The startup configuration area in the flash; config_read() reads it.
#define CONFIG_START 0x70000
#define CONFIG_LEN 0x1000
#if CONFIG_LEN % CONFIG_PAGE_SIZE
#error "CONFIG_LEN not a multiple of CONFIG_PAGE_SIZE"
#endif


void flash_init(uint8_t enable_dio);
void flash_dump(uint8_t len);
void flash_read_uid(void);
void flash_read_jedecid(void);
void flash_read_security(void);
void flash_read_bulk(__xdata uint8_t *dst);
void flash_sector_erase(void);
void flash_write_bytes(__xdata uint8_t *ptr);

#endif

