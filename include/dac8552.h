/**
 * @file dac8552.h
 * @brief TI DAC8552 dual 16-bit unbuffered SPI DAC driver.
 *
 * Per TI datasheet SBAS362F. The chip is write-only over SPI (no DOUT),
 * 24-bit frames clocked MSB-first, ~SYNC active LOW. SPI mode 1
 * (CPOL=0, CPHA=1).
 *
 * 24-bit frame layout:
 *   DB23..DB16  control byte (see ::DAC8552LoadMode + ::DAC8552Channel +
 *                              ::DAC8552PowerMode)
 *   DB15..DB0   16-bit unsigned code, 0..0xFFFF -> 0..V_REF
 *
 * Control-byte bit positions (table 1):
 *   bit 7 (DB23) : LD1   } load-control
 *   bit 6 (DB22) : LD0   } LD1:LD0 = 00 -> stage in buffer only
 *                          LD1:LD0 = 10 -> load THIS DAC on rising ~SYNC
 *                          LD1:LD0 = 11 -> load BOTH DACs on rising ~SYNC
 *   bit 5 (DB21) : X
 *   bit 4 (DB20) : BUF_SEL  (0 = buffer A, 1 = buffer B)
 *   bit 3 (DB19) : X
 *   bit 2 (DB18) : X
 *   bit 1 (DB17) : PD1   } power: 00 = active, 01 = 1k to GND,
 *   bit 0 (DB16) : PD0     10 = 100k to GND, 11 = Hi-Z
 *
 * The driver is HAL-injected: the application supplies a ``spiWrite``
 * function pointer that clocks 3 bytes out on MOSI with ~SYNC handled
 * by the platform (e.g. the IDF SPI device's CS line). The driver
 * never touches GPIO directly.
 *
 * @author Orion Serup <orion@crablabs.io>
 */

#pragma once
#ifndef DAC8552_H
#define DAC8552_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Driver result code. 0 = success, negative = error. */
typedef enum
{
	DAC8552_ERROR_OK              = 0,
	DAC8552_ERROR_NULL_PARAM      = -1,
	DAC8552_ERROR_NOT_INITIALIZED = -2,
	DAC8552_ERROR_SPI             = -3,
	DAC8552_ERROR_INVALID_PARAM   = -4,
} DAC8552Error;

/** @brief Output channel selector. */
typedef enum
{
	DAC8552_CHANNEL_A = 0,
	DAC8552_CHANNEL_B = 1,
} DAC8552Channel;

/** @brief LD1:LD0 field encoding, pre-shifted to bits 7:6 of the
 *         control byte so they can be OR-ed in directly. */
typedef enum
{
	DAC8552_LOAD_BUFFER_ONLY = 0x00U, ///< Stage in input buffer, no DAC update.
	DAC8552_LOAD_THIS        = 0x80U, ///< Update THIS DAC on rising ~SYNC.
	DAC8552_LOAD_ALL         = 0xC0U, ///< Update BOTH DACs on rising ~SYNC.
} DAC8552LoadMode;

/** @brief PD1:PD0 field encoding, pre-shifted to bits 1:0. */
typedef enum
{
	DAC8552_POWER_ACTIVE  = 0x00U, ///< Normal operating mode.
	DAC8552_POWER_1K_GND  = 0x01U, ///< Output: 1 kOhm to GND.
	DAC8552_POWER_100K_GND= 0x02U, ///< Output: 100 kOhm to GND.
	DAC8552_POWER_HIZ     = 0x03U, ///< Output: Hi-Z (boot default).
} DAC8552PowerMode;

/** @brief HAL adapter the driver calls into. The application provides
 *         the SPI side; the driver does not know about CS lines, DMA,
 *         queues, or anything else platform-specific. */
typedef struct
{
	/**
	 * @brief Clock @p length_bytes bytes of @p data out on the chip's
	 *        MOSI line, with ~SYNC asserted around the whole call.
	 *
	 * @return 0 on success, non-zero on hardware failure.
	 */
	int (*spiWrite)(const void* const data, const uint8_t length_bytes);
} DAC8552HAL;

/** @brief Device handle. Hold one of these per chip on the bus. */
typedef struct
{
	DAC8552HAL hal;
	bool is_initialized;
} DAC8552;

/**
 * @brief Stash the HAL function pointers into @p dev. Does NOT touch the
 *        chip -- bring-up sequences (wake from Hi-Z, mid-rail park) are
 *        left to the application so it can decide the safe initial
 *        state for the downstream analog stage.
 *
 * @return ::DAC8552_ERROR_OK on success;
 *         ::DAC8552_ERROR_NULL_PARAM if either argument is NULL or if
 *         @p hal->spiWrite is NULL.
 */
DAC8552Error dac8552Init(DAC8552* const dev, const DAC8552HAL* const hal);

/**
 * @brief Write one 24-bit frame: control byte (load mode + channel +
 *        power mode) followed by the 16-bit code.
 *
 * @param[in] ch          Which channel's INPUT buffer to write.
 * @param[in] load_mode   When the chip should latch the buffer to the
 *                        output.
 * @param[in] power_mode  Power-down state for this channel.
 * @param[in] code        Unsigned 16-bit code; 0 -> 0 V, 0xFFFF -> V_REF.
 *
 * @return ::DAC8552_ERROR_OK on success;
 *         ::DAC8552_ERROR_NOT_INITIALIZED if ::dac8552Init has not run;
 *         ::DAC8552_ERROR_SPI if the HAL spiWrite returned non-zero.
 */
DAC8552Error dac8552WriteCode(const DAC8552* const dev,
                              const DAC8552Channel ch,
                              const DAC8552LoadMode load_mode,
                              const DAC8552PowerMode power_mode,
                              const uint16_t code);

/**
 * @brief Glitch-free synchronous update of both outputs. Stages @p code_a
 *        in buffer A (no DAC update), then writes @p code_b to buffer B
 *        with LOAD_ALL so both outputs change in lock-step on the rising
 *        ~SYNC at the end of the B frame.
 *
 *        This is the right primitive for differential / push-pull drive:
 *        the load points are coincident at the chip and the analog stage
 *        sees no intermediate half-step.
 *
 * @return ::DAC8552_ERROR_OK on success; the first non-OK code if either
 *         frame failed (no rollback -- the chip is in whatever state the
 *         partial sequence left it).
 */
DAC8552Error dac8552WriteBothSync(const DAC8552* const dev,
                                  const uint16_t code_a,
                                  const uint16_t code_b);

#ifdef __cplusplus
}
#endif

#endif /* DAC8552_H */
