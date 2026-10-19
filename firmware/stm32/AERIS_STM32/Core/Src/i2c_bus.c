/* i2c_bus.c - I2C1 access with timeouts and bus recovery (see i2c_bus.h). */
#include "i2c_bus.h"
#include "aeris_config.h"
#include "main.h"

static i2c_bus_stats_t stats;

static void short_delay(void)
{
    /* ~5 us at 84 MHz: half an SCL period at 100 kHz. */
    for (volatile uint32_t i = 0; i < 70U; i++) {
    }
}

static void handle_failure(HAL_StatusTypeDef st)
{
    stats.errors++;
    /* NACK (HAL_ERROR with AF) means the device didn't answer: the bus
     * itself is fine. BUSY/TIMEOUT usually means SDA is held low. */
    if (st == HAL_BUSY || st == HAL_TIMEOUT ||
        (HAL_I2C_GetError(&hi2c1) & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_ARLO))) {
        i2c_bus_recover();
    }
}

bool i2c_bus_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(addr7 << 1), reg,
                                            I2C_MEMADD_SIZE_8BIT, buf, len, I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        handle_failure(st);
        return false;
    }
    return true;
}

bool i2c_bus_write(uint8_t addr7, uint8_t reg, uint8_t value)
{
    HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(addr7 << 1), reg,
                                             I2C_MEMADD_SIZE_8BIT, &value, 1, I2C_TIMEOUT_MS);
    if (st != HAL_OK) {
        handle_failure(st);
        return false;
    }
    return true;
}

bool i2c_bus_probe(uint8_t addr7)
{
    return HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr7 << 1), 2, I2C_TIMEOUT_MS) == HAL_OK;
}

void i2c_bus_recover(void)
{
    GPIO_InitTypeDef g = {0};

    stats.recoveries++;
    HAL_I2C_DeInit(&hi2c1);

    /* Drive SCL (PB8) as open-drain GPIO, read SDA (PB9). */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Pin   = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode  = GPIO_MODE_OUTPUT_OD;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_SET);
    HAL_GPIO_Init(GPIOB, &g);
    short_delay();

    /* Up to 9 clocks lets a slave finish the byte it is stuck in. */
    for (int i = 0; i < 9; i++) {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET) {
            break;
        }
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
        short_delay();
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
        short_delay();
    }

    /* Generate a STOP: SDA low -> high while SCL is high. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    short_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    short_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    short_delay();

    /* Reset the peripheral itself, then restore AF pins via MspInit. */
    __HAL_RCC_I2C1_FORCE_RESET();
    __HAL_RCC_I2C1_RELEASE_RESET();
    (void)HAL_I2C_Init(&hi2c1);
}

void i2c_bus_get_stats(i2c_bus_stats_t *out)
{
    *out = stats;
}
