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
#include "rgb.h"

#include "stm32f4xx_hal.h"
#include "tusb.h"

// Words at the top of RAM that survive a reset, see `linker/stm32f405xg.ld`
extern uint32_t _board_rescue[];
extern uint32_t _board_bootloader_flag[];
#define BOARD_RESCUE_MAGIC_WORD _board_rescue[0]
#define BOARD_FAILED_BOOTS _board_rescue[1]
#define BOARD_BOOT_COUNT _board_rescue[2]
#define BOARD_BOOTLOADER_FLAG _board_bootloader_flag[0]

// Marks the counters as valid. RAM holds random values after power-up.
#define BOARD_RESCUE_MAGIC 0x52534355

// Reset flags of this boot (top byte of RCC_CSR), reported in the serial number
static uint8_t board_reset_flags;

/**
 * @brief Reset into the vendor bootloader
 *
 * The bootloader stays in IAP mode when it finds `BOOTLOADER_MAGIC` at the end
 * of RAM after a reset (verified on the MAD60 HE for both a software reset and
 * a watchdog reset). This needs no clocks, peripherals or USB, so it is safe to
 * call from any state, including fault handlers.
 *
 * @return None
 */
__attribute__((noreturn)) static void board_rescue(void) {
  __disable_irq();
  BOARD_BOOTLOADER_FLAG = BOOTLOADER_MAGIC;
  // Start counting from zero again once a firmware has been flashed
  BOARD_RESCUE_MAGIC_WORD = 0;
  __DSB();
  NVIC_SystemReset();
}

/**
 * @brief Bring the chip back to its reset state
 *
 * The vendor bootloader may jump here with its clocks, SysTick and USB
 * interrupts still running. The stock firmware resets the same way.
 *
 * @return None
 */
static void board_quiesce(void) {
  __disable_irq();
  SysTick->CTRL = 0;
  for (uint32_t i = 0; i < M_ARRAY_SIZE(NVIC->ICER); i++) {
    NVIC->ICER[i] = 0xFFFFFFFF;
    NVIC->ICPR[i] = 0xFFFFFFFF;
  }

  // Pulse every peripheral reset, as the stock firmware does before it enters
  // the bootloader (FUN_08026a24 in v1.06)
  RCC->AHB1RSTR = 0xFFFFFFFF;
  RCC->AHB1RSTR = 0;
  RCC->AHB2RSTR = 0xFFFFFFFF;
  RCC->AHB2RSTR = 0;
  RCC->APB1RSTR = 0xFFFFFFFF;
  RCC->APB1RSTR = 0;
  RCC->APB2RSTR = 0xFFFFFFFF;
  RCC->APB2RSTR = 0;

  // Run from HSI with the PLL off, so that HAL starts from a known clock state
  RCC->CR |= RCC_CR_HSION;
  while (!(RCC->CR & RCC_CR_HSIRDY))
    ;
  RCC->CFGR = 0;
  while (RCC->CFGR & RCC_CFGR_SWS)
    ;
  RCC->CR &= ~RCC_CR_PLLON;
  while (RCC->CR & RCC_CR_PLLRDY)
    ;

  // Nothing is enabled any more, so interrupts can be unmasked for HAL
  __enable_irq();
}

/**
 * @brief Count this boot and start the watchdog
 *
 * A boot counts as failed until USB is configured. After
 * `BOARD_RESCUE_MAX_FAILED_BOOTS` failed boots in a row the board goes to the
 * bootloader, so a firmware that hangs or never enumerates can be replaced
 * over USB without a debugger.
 *
 * @return None
 */
static void board_rescue_init(void) {
  board_reset_flags = (uint8_t)(RCC->CSR >> 24);
  RCC->CSR |= RCC_CSR_RMVF;

  // Our previous boot left the counters (and armed the watchdog). RAM survives
  // a pass through the vendor bootloader (checked on the MAD60 HE, 2026-10-17).
  // It can also survive a short power dip, such as a laptop switching its USB
  // power while it boots, so a power-on or brown-out reset starts from zero:
  // otherwise two dips before USB is configured send the board to the
  // bootloader.
  const bool power_on =
      board_reset_flags & ((RCC_CSR_PORRSTF | RCC_CSR_BORRSTF) >> 24);
  const bool counters_valid =
      !power_on && BOARD_RESCUE_MAGIC_WORD == BOARD_RESCUE_MAGIC;
  if (!counters_valid) {
    BOARD_RESCUE_MAGIC_WORD = BOARD_RESCUE_MAGIC;
    BOARD_FAILED_BOOTS = 0;
    BOARD_BOOT_COUNT = 0;
  }
  BOARD_BOOT_COUNT++;

#if BOARD_RESCUE_ON_WATCHDOG
  // RCC_CSR flags are sticky until cleared, and the stock firmware never clears
  // them, so only trust IWDGRSTF when our own previous boot armed the watchdog.
  // Without that check the first boot after flashing went to the bootloader
  // because of a watchdog reset from earlier stock testing.
  if (counters_valid && (board_reset_flags & (RCC_CSR_IWDGRSTF >> 24)))
    board_rescue();
#endif
  if (BOARD_FAILED_BOOTS >= BOARD_RESCUE_MAX_FAILED_BOOTS)
    board_rescue();
  BOARD_FAILED_BOOTS++;

  // IWDG on LSI (about 32 kHz) / 32, about 1 ms per count
  _Static_assert(BOARD_RESCUE_WATCHDOG_MS >= 1 && BOARD_RESCUE_WATCHDOG_MS <= 4095,
                 "Invalid watchdog timeout");
  IWDG->KR = 0xCCCC;
  IWDG->KR = 0x5555;
  IWDG->PR = 3; // divide by 32
  IWDG->RLR = BOARD_RESCUE_WATCHDOG_MS;
  while (IWDG->SR)
    ;
  IWDG->KR = 0xAAAA;
}

/**
 * @brief Initialize the clock
 *
 * HSE crystal, 168 MHz system clock, 48 MHz USB clock from the main PLL. Same
 * settings as the stock firmware (PLLM 16, N 336, P 2, Q 7).
 *
 * @return None
 */
static void board_clock_init(void) {
  RCC_OscInitTypeDef rcc_osc_init = {0};
  RCC_ClkInitTypeDef rcc_clk_init = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  rcc_osc_init.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  rcc_osc_init.HSEState = RCC_HSE_ON;
  rcc_osc_init.PLL.PLLState = RCC_PLL_ON;
  rcc_osc_init.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  rcc_osc_init.PLL.PLLM = BOARD_HSE_VALUE / 1000000;
  rcc_osc_init.PLL.PLLN = 336;
  rcc_osc_init.PLL.PLLP = RCC_PLLP_DIV2;
  rcc_osc_init.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&rcc_osc_init) != HAL_OK)
    board_error_handler();

  rcc_clk_init.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                           RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  rcc_clk_init.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  rcc_clk_init.AHBCLKDivider = RCC_SYSCLK_DIV1;
  rcc_clk_init.APB1CLKDivider = RCC_HCLK_DIV4;
  rcc_clk_init.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&rcc_clk_init, FLASH_LATENCY_5) != HAL_OK)
    board_error_handler();
}

/**
 * @brief Initialize the USB
 *
 * OTG_HS core with its embedded full-speed PHY on PB14 (D-) / PB15 (D+), no
 * VBUS sensing.
 *
 * @return None
 */
static void board_usb_init(void) {
  GPIO_InitTypeDef gpio_init = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  // USB D+/D-
  gpio_init.Pin = GPIO_PIN_14 | GPIO_PIN_15;
  gpio_init.Mode = GPIO_MODE_AF_PP;
  gpio_init.Pull = GPIO_NOPULL;
  gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio_init.Alternate = GPIO_AF12_OTG_HS_FS;
  HAL_GPIO_Init(GPIOB, &gpio_init);

  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

  // Disable VBUS sensing (this OTG core version has NOVBUSSENS, not VBDEN)
  USB_OTG_HS->GCCFG |= USB_OTG_GCCFG_NOVBUSSENS;
  USB_OTG_HS->GCCFG &= ~(USB_OTG_GCCFG_VBUSBSEN | USB_OTG_GCCFG_VBUSASEN);
}

void board_init(void) {
  board_quiesce();

  // The bootloader has already seen this flag if it was meant for it. Never act
  // on it here, or a flag the bootloader leaves behind would bounce every boot
  // back into the bootloader.
  if (BOARD_BOOTLOADER_FLAG == BOOTLOADER_MAGIC)
    BOARD_BOOTLOADER_FLAG = 0;

  board_rescue_init();

  HAL_Init();

  board_clock_init();
  board_usb_init();

  // Enable cycle counter
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  DWT->CYCCNT = 0;
}

void board_task(void) {
  IWDG->KR = 0xAAAA;

#if defined(RGB_NUM_LEDS)
  rgb_task();
#endif

  if (BOARD_FAILED_BOOTS == 0)
    return;
  if (tud_mounted())
    // This boot worked
    BOARD_FAILED_BOOTS = 0;
#if BOARD_RESCUE_NO_USB_MS > 0
  else if (HAL_GetTick() >= BOARD_RESCUE_NO_USB_MS)
    // Straight to the bootloader: needs only the flag word, nothing else in RAM
    board_rescue();
#endif
}

void board_error_handler(void) { board_rescue(); }

void board_reset(void) { NVIC_SystemReset(); }

void board_enter_bootloader(void) { board_rescue(); }

bool board_read_memory(uint32_t addr, uint32_t len, uint8_t *out) {
  // Only regions that read without a fault. Registers are read as whole words.
  static const struct {
    uint32_t start, end;
    bool word;
  } regions[] = {
      {0x08000000, 0x08020000, false}, // sectors 0-4: vendor bootloader, stock
                                       // settings
      {0x1FFF0000, 0x1FFF7A24, false}, // system memory, OTP, UID, flash size
      {0x1FFFC000, 0x1FFFC010, false}, // option bytes
      {0x40023C14, 0x40023C18, true},  // FLASH_OPTCR
      {0xE0042000, 0xE0042004, true},  // DBGMCU_IDCODE
  };

  for (uint32_t i = 0; i < M_ARRAY_SIZE(regions); i++) {
    if (addr < regions[i].start || addr >= regions[i].end ||
        len > regions[i].end - addr)
      continue;
    if (!regions[i].word) {
      memcpy(out, (const void *)addr, len);
      return true;
    }
    if (addr % 4 != 0 || len % 4 != 0)
      return false;
    for (uint32_t j = 0; j < len; j += 4) {
      const uint32_t v = *(volatile const uint32_t *)(addr + j);
      memcpy(out + j, &v, 4);
    }
    return true;
  }
  return false;
}

#if defined(BOARD_HAS_BOOTLOADER_INSTALL)
// New bootloader image, staged over raw HID and then written in one go so that
// the sector is without boot code for as short a time as possible
static uint8_t board_boot_image[0x4000] __attribute__((aligned(4)));

bool board_stage_bootloader(uint32_t offset, uint32_t len, const uint8_t *data) {
  if (offset >= sizeof(board_boot_image) || len > sizeof(board_boot_image) - offset)
    return false;
  memcpy(&board_boot_image[offset], data, len);
  return true;
}

// CRC-32 as zlib computes it, so the host can check with zlib.crc32
static uint32_t board_crc32(const uint8_t *p, uint32_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (uint32_t b = 0; b < 8; b++)
      crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  }
  return ~crc;
}

bool board_install_bootloader(uint32_t len, uint32_t crc) {
  const uint32_t *image = (const uint32_t *)board_boot_image;

  // Stack in the first 128 KB of RAM, Thumb reset vector inside the image
  if (len < 8 || len > sizeof(board_boot_image) || len % 4 != 0 ||
      board_crc32(board_boot_image, len) != crc ||
      (image[0] & 0x2FFE0000) != 0x20000000 || !(image[1] & 1) ||
      image[1] < FLASH_BASE + 8 || image[1] >= FLASH_BASE + len)
    return false;

  FLASH_EraseInitTypeDef erase = {0};
  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.Banks = FLASH_BANK_1;
  erase.Sector = 0;
  erase.NbSectors = 1;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

  bool ok = false;
  for (uint32_t attempt = 0; !ok && attempt < 3; attempt++) {
    uint32_t error = 0;
    // Nothing else may run in between. HAL's flash timeouts never expire with
    // the tick stopped, so these wait for the controller as long as it takes.
    IWDG->KR = 0xAAAA;
    __disable_irq();
    HAL_FLASH_Unlock();
    ok = HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
    IWDG->KR = 0xAAAA;
    for (uint32_t i = 0; ok && i < len / 4; i++)
      ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_BASE + i * 4, image[i]) ==
           HAL_OK;
    HAL_FLASH_Lock();
    __enable_irq();
    IWDG->KR = 0xAAAA;
    // The erase flushed the data cache, so this reads the flash itself
    ok = ok && memcmp((const void *)FLASH_BASE, board_boot_image, len) == 0;
  }
  return ok;
}
#endif

uint32_t board_serial(char *buf) {
  const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
  // Use the 96-bit unique ID as the serial number
  for (uint32_t i = 0; i < 12; i++) {
    buf[i * 2] = M_HEX(uid[i] >> 4);
    buf[i * 2 + 1] = M_HEX(uid[i] & 0x0F);
  }

#if BOARD_SERIAL_DIAGNOSTICS
  // Bring-up: "-BB-FF", boots since power-up and the RCC_CSR reset flags. Shows
  // whether RAM and the reset flags survive a pass through the bootloader.
  const uint8_t diag[] = {(uint8_t)BOARD_BOOT_COUNT, board_reset_flags};
  for (uint32_t i = 0; i < 2; i++) {
    buf[24 + i * 3] = '-';
    buf[25 + i * 3] = M_HEX(diag[i] >> 4);
    buf[26 + i * 3] = M_HEX(diag[i] & 0x0F);
  }
  return 30;
#else
  return 24;
#endif
}

uint32_t board_cycle_count(void) { return DWT->CYCCNT; }

//--------------------------------------------------------------------+
// Interrupt Handlers
//--------------------------------------------------------------------+

void SysTick_Handler(void) { HAL_IncTick(); }

void OTG_HS_IRQHandler(void) { tud_int_handler(BOARD_TUD_RHPORT); }

// MemManage, BusFault and UsageFault are disabled by default and escalate here
void HardFault_Handler(void) { board_rescue(); }
