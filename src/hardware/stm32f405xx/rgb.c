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

#if defined(RGB_NUM_LEDS)

// WS2812 chain on PC9 = TIM3_CH4 (AF2), fed by DMA1 stream 2 channel 5 in
// circular mode, the same as the stock MAD60 HE firmware: 84 MHz timer, 12.5 us
// per bit, high for 0.35 us ("0") or 0.80 us ("1"). WS2812 decodes the high
// time, so the long low part only slows the frame down (~21 ms). The trailing
// zero slots hold the line low between frames (reset).
#define RGB_BIT_PERIOD 1050
#define RGB_BIT_0 29
#define RGB_BIT_1 67
#define RGB_RESET_SLOTS 224
#define RGB_NUM_SLOTS (RGB_NUM_LEDS * 24 + RGB_RESET_SLOTS)

static TIM_HandleTypeDef rgb_tim;
static DMA_HandleTypeDef rgb_dma;
// Read continuously by the DMA; LEDs are written in place
static uint16_t rgb_slots[RGB_NUM_SLOTS];
// Colour last written per LED (GRB), to skip unchanged LEDs: static effects
// then cost almost nothing per frame. All LEDs start off, as the slots do.
static uint32_t rgb_last[RGB_NUM_LEDS];

void rgb_driver_init(void) {
  GPIO_InitTypeDef gpio_init = {0};
  TIM_OC_InitTypeDef oc_init = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  // Every LED off, then the reset slots stay 0
  for (uint32_t i = 0; i < RGB_NUM_SLOTS; i++)
    rgb_slots[i] = i < RGB_NUM_LEDS * 24 ? RGB_BIT_0 : 0;

  gpio_init.Pin = GPIO_PIN_9;
  gpio_init.Mode = GPIO_MODE_AF_PP;
  gpio_init.Pull = GPIO_NOPULL;
  gpio_init.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio_init.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOC, &gpio_init);

  rgb_tim.Instance = TIM3;
  rgb_tim.Init.Prescaler = 0;
  rgb_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  rgb_tim.Init.Period = RGB_BIT_PERIOD - 1;
  rgb_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  rgb_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_PWM_Init(&rgb_tim) != HAL_OK)
    board_error_handler();

  oc_init.OCMode = TIM_OCMODE_PWM1;
  oc_init.Pulse = 0;
  oc_init.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc_init.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&rgb_tim, &oc_init, TIM_CHANNEL_4) != HAL_OK)
    board_error_handler();

  rgb_dma.Instance = DMA1_Stream2;
  rgb_dma.Init.Channel = DMA_CHANNEL_5;
  rgb_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
  rgb_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  rgb_dma.Init.MemInc = DMA_MINC_ENABLE;
  rgb_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  rgb_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  rgb_dma.Init.Mode = DMA_CIRCULAR;
  rgb_dma.Init.Priority = DMA_PRIORITY_LOW;
  rgb_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&rgb_dma) != HAL_OK)
    board_error_handler();
  __HAL_LINKDMA(&rgb_tim, hdma[TIM_DMA_ID_CC4], rgb_dma);

  // No DMA interrupt is enabled in the NVIC: the circular transfer runs on its
  // own and nothing waits for it
  if (HAL_TIM_PWM_Start_DMA(&rgb_tim, TIM_CHANNEL_4, (uint32_t *)rgb_slots,
                            RGB_NUM_SLOTS) != HAL_OK)
    board_error_handler();
}

void rgb_driver_set(uint8_t led, uint8_t r, uint8_t g, uint8_t b) {
  if (led >= RGB_NUM_LEDS)
    return;

  // WS2812 order: green, red, blue, most significant bit first
  const uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
  if (grb == rgb_last[led])
    return;
  rgb_last[led] = grb;
  uint16_t *slot = &rgb_slots[led * 24];
  for (uint32_t i = 0; i < 24; i++)
    slot[i] = (grb >> (23 - i)) & 1 ? RGB_BIT_1 : RGB_BIT_0;
}

#endif
