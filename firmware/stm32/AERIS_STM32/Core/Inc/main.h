/* main.h - board-level declarations (STM32CubeMX layout). */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Nucleo-F446RE pin map used by AERIS
 *   LD2 (green LED) ........ PA5
 *   I2C1 SCL (D15) ......... PB8   -> MPU6500 SCL, BMP280 SCK
 *   I2C1 SDA (D14) ......... PB9   -> MPU6500 SDA, BMP280 SDI
 *   USART1 TX (D8) ......... PA9   -> ESP8266 RX  (GPIO13 / D7 after Serial.swap())
 *   USART1 RX (D2) ......... PA10  <- ESP8266 TX  (GPIO15 / D8 after Serial.swap())
 *   USART2 TX/RX ........... PA2/PA3 -> ST-LINK virtual COM port (debug log)
 */
#define LD2_Pin        GPIO_PIN_5
#define LD2_GPIO_Port  GPIOA

extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
