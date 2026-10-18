/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "hardware/hardware.h"

#include "stm32f4xx_hal.h"

// Only the wear leveling sectors at the end of flash may ever be erased or
// written. Below them are this firmware, the stock firmware's settings
// (sectors 1-3) and the vendor bootloader (sector 0), which has no backup.

void flash_init(void) {}

/**
 * @brief Get the offset of a sector from the start of flash
 *
 * @param sector Sector index
 *
 * @return Offset in bytes
 */
static uint32_t flash_sector_offset(uint32_t sector) {
  uint32_t offset = 0;
  for (uint32_t i = 0; i < sector; i++)
    offset += flash_sector_size(i);
  return offset;
}

bool flash_erase(uint32_t sector) {
  if (sector >= FLASH_NUM_SECTORS || flash_sector_offset(sector) < WL_BASE_ADDRESS)
    return false;

  FLASH_EraseInitTypeDef erase = {0};
  uint32_t error = 0;
  bool success = true;

  HAL_FLASH_Unlock();
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Banks = FLASH_BANK_1;
  erase.Sector = sector;
  erase.NbSectors = 1;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  success = (HAL_FLASHEx_Erase(&erase, &error) == HAL_OK);
  HAL_FLASH_Lock();

  return success;
}

bool flash_read(uint32_t addr, void *buf, uint32_t len) {
  if (addr + len * 4 > FLASH_SIZE)
    return false;

  memcpy(buf, (void *)(FLASH_BASE + addr), len * 4);

  return true;
}

bool flash_write(uint32_t addr, const void *buf, uint32_t len) {
  if (addr < WL_BASE_ADDRESS || addr + len * 4 > FLASH_SIZE)
    return false;

  const uint32_t *buf32 = buf;

  HAL_FLASH_Unlock();
  for (uint32_t i = 0; i < len; i++) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_BASE + addr + i * 4,
                          buf32[i]) != HAL_OK) {
      HAL_FLASH_Lock();
      return false;
    }
  }
  HAL_FLASH_Lock();

  return true;
}
