/**
 * @file dac8552.h
 * @brief TI DAC8552 dual 16-bit unbuffered SPI DAC driver.
 *
 * Per TI datasheet SBAS362F. The chip is write-only over SPI (no DOUT),
 * 24-bit frames clocked MSB-first, ~SYNC active LOW. SPI mode 1
 * (CPOL=0, CPHA=1). Two output channels A / B, each with an input
 * buffer and a DAC register; LD1:LD0 in the control byte selects which
 * (if any) DAC registers are latched from the buffers on the rising
 * ~SYNC at the end of each frame.
 *
 * Frame layout (DB23..DB0, MSB first):
 *   DB23..DB16  control byte (load + buffer + power, see enums)
 *   DB15..DB0   16-bit unsigned code, 0..0xFFFF -> 0..V_REF
 *
 * Control-byte bit positions (SLAS430A Figure 43 / Table 1):
 *   bit 7 (DB23) : RESERVED (must be 0)
 *   bit 6 (DB22) : RESERVED (must be 0)
 *   bit 5 (DB21) : LDB  -- 1 = load DAC B from buffer B
 *   bit 4 (DB20) : LDA  -- 1 = load DAC A from buffer A
 *   bit 3 (DB19) : DON'T CARE
 *   bit 2 (DB18) : BUF_SEL (0 = data written to buffer A, 1 = buffer B)
 *   bit 1 (DB17) : PD1   } 00 = active, 01 = 1k to GND,
 *   bit 0 (DB16) : PD0     10 = 100k to GND, 11 = Hi-Z
 *
 * The driver is HAL-injected: the application supplies a ``spiWrite``
 * function pointer that clocks 3 bytes out on MOSI with ~SYNC handled
 * by the platform (e.g. the IDF SPI device's CS line). The driver
 * never touches GPIO directly.
 *
 * API layering:
 *
 *   1. Lifecycle
 *        dac8552Init             stash HAL, no chip touch.
 *
 *   2. Power-mode control
 *        dac8552Configure        per-channel power mode in one
 *                                synchronous LOAD_ALL frame pair (also
 *                                wakes both channels from boot Hi-Z).
 *                                Takes the two power modes directly.
 *        dac8552SetPowerMode     change ONE channel's power mode in
 *                                place, no DAC update.
 *
 *   3. Buffer staging (no DAC update)
 *        dac8552WriteBuffer      stage one channel's input buffer.
 *
 *   4. Output latch
 *        dac8552Write            write ONE channel's buffer AND load that
 *                                channel's DAC register (LDA or LDB).
 *        dac8552WriteAll         write BOTH channels (independent codes)
 *                                in one synchronous LOAD_ALL frame pair
 *                                -- the glitch-free push-pull primitive.
 *        dac8552LatchChannel     latch ONE channel's DAC register from
 *                                its current input buffer (buffer is
 *                                not disturbed).
 *        dac8552LatchAll         latch BOTH DAC registers from their
 *                                current input buffers (neither buffer
 *                                is disturbed).
 *
 *   5. Read-back (driver-side cache; the chip has no DOUT)
 *        dac8552GetLastValue     return the last code commanded for
 *                                a channel (whether via Write, WriteAll,
 *                                or WriteBuffer).
 *        dac8552GetPowerMode     return a channel's current power mode.
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
	DAC8552_CHANNEL_COUNT = 2,
} DAC8552Channel;

/** @brief DAC-register load mode encoded as the LDA/LDB bits in the
 *         control byte (SLAS430A Figure 43 + Table 1):
 *             DB21 = LDB (bit 5) -- 1 = load DAC B from buffer B
 *             DB20 = LDA (bit 4) -- 1 = load DAC A from buffer A
 *         DB23, DB22 are RESERVED and must be 0. Internal use; public
 *         callers pick a higher-level entry point. */
typedef enum
{
	DAC8552_LOAD_BUFFER_ONLY = 0x00U, ///< LDA=0, LDB=0 -- buffer write, no DAC update.
	DAC8552_LOAD_A           = 0x10U, ///< LDA=1 -- load DAC A from buffer A.
	DAC8552_LOAD_B           = 0x20U, ///< LDB=1 -- load DAC B from buffer B.
	DAC8552_LOAD_ALL         = 0x30U, ///< LDA=1, LDB=1 -- load both.
} DAC8552LoadMode;

/** @brief PD1:PD0 field encoding, pre-shifted to bits 1:0. */
typedef enum
{
	DAC8552_POWER_ACTIVE   = 0x00U, ///< Normal operating mode.
	DAC8552_POWER_1K_GND   = 0x01U, ///< Output: 1 kOhm to GND.
	DAC8552_POWER_100K_GND = 0x02U, ///< Output: 100 kOhm to GND.
	DAC8552_POWER_HIZ      = 0x03U, ///< Output: Hi-Z (boot default).
} DAC8552PowerMode;

/** @brief HAL adapter the driver calls into. */
typedef struct
{
	/**
	 * @brief Clock @p length_bytes bytes of @p data out on the chip's
	 *        MOSI line, with ~SYNC asserted around the whole call.
	 * @return 0 on success, non-zero on hardware failure.
	 */
	int (*spiWrite)(const void* const data, const uint8_t length_bytes);
} DAC8552HAL;

/** @brief Device handle. One per chip on the bus. Power mode + last
 *         commanded value per channel live here -- the chip itself has
 *         no DOUT, so the driver-side cache IS the truth. */
typedef struct
{
	DAC8552HAL       hal;
	bool             is_initialized;
	DAC8552PowerMode power_mode[DAC8552_CHANNEL_COUNT];
	uint16_t         last_value[DAC8552_CHANNEL_COUNT];
} DAC8552;

/* -- 1. Lifecycle ------------------------------------------------------- */

/**
 * @brief Stash the HAL function pointers into @p dev and seed the
 *        driver cache to the chip's boot defaults (Hi-Z, code 0 on
 *        both channels). Does NOT touch the chip. Call
 *        ::dac8552Configure next to wake from Hi-Z.
 *
 * @return ::DAC8552_ERROR_OK on success;
 *         ::DAC8552_ERROR_NULL_PARAM if either argument is NULL or if
 *         @p hal->spiWrite is NULL.
 */
DAC8552Error dac8552Init(DAC8552* const dev, const DAC8552HAL* const hal);

/* -- 2. Power-mode control --------------------------------------------- */

/**
 * @brief Apply per-channel power mode in one synchronous LOAD_ALL
 *        frame pair. Also wakes both channels from the boot-default
 *        Hi-Z state. Input buffers and DAC registers latch to 0.
 */
DAC8552Error dac8552Configure(DAC8552* const dev,
                              const DAC8552PowerMode power_a,
                              const DAC8552PowerMode power_b);

/**
 * @brief Change ONE channel's power mode in place. Sends a single
 *        buffer-only frame with the new PD field; the other channel
 *        is untouched. The DAC output for @p ch does NOT latch.
 */
DAC8552Error dac8552SetPowerMode(DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 const DAC8552PowerMode mode);

/* -- 3. Buffer staging (no DAC update) --------------------------------- */

/**
 * @brief Stage @p code in one channel's input buffer WITHOUT updating
 *        the DAC output. Pair with ::dac8552LatchChannel or
 *        ::dac8552LatchAll to apply later.
 */
DAC8552Error dac8552WriteBuffer(DAC8552* const dev,
                                const DAC8552Channel ch,
                                const uint16_t code);

/* -- 4. Output latch --------------------------------------------------- */

/**
 * @brief Write ONE channel's input buffer AND latch it immediately
 *        (LDA/LDB). The other channel is untouched.
 */
DAC8552Error dac8552Write(DAC8552* const dev,
                          const DAC8552Channel ch,
                          const uint16_t code);

/**
 * @brief Write BOTH channels (independent codes) in one synchronous
 *        LOAD_ALL frame pair. Stages A in its buffer, then writes B
 *        with LOAD_ALL so both DAC registers latch in lock-step on
 *        the rising ~SYNC at the end of the B frame.
 *
 *        Glitch-free primitive for push-pull / differential drive.
 *        Pass @p code_a == @p code_b to park both channels together.
 */
DAC8552Error dac8552WriteAll(DAC8552* const dev,
                             const uint16_t code_a,
                             const uint16_t code_b);

/**
 * @brief Latch ONE channel's DAC register from its current input
 *        buffer (LDA/LDB). Re-emits the cached buffer code so the
 *        buffer is not disturbed -- only the DAC register updates.
 */
DAC8552Error dac8552LatchChannel(DAC8552* const dev,
                                 const DAC8552Channel ch);

/**
 * @brief Latch BOTH DAC registers from their current input buffers
 *        (LOAD_ALL). Re-emits the cached buffer codes so neither
 *        buffer is disturbed -- both DAC registers update
 *        simultaneously.
 */
DAC8552Error dac8552LatchAll(DAC8552* const dev);

/* -- 5. Read-back (driver-side cache) ---------------------------------- */

/**
 * @brief Return the last code commanded for @p ch (via any write or
 *        buffer-staging call). The chip has no DOUT -- this driver
 *        cache IS the answer.
 */
DAC8552Error dac8552GetLastValue(const DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 uint16_t* const out_code);

/**
 * @brief Return @p ch's current power mode (as set by ::dac8552Init,
 *        ::dac8552Configure, or ::dac8552SetPowerMode).
 */
DAC8552Error dac8552GetPowerMode(const DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 DAC8552PowerMode* const out_mode);

#ifdef __cplusplus
}
#endif

#endif /* DAC8552_H */
