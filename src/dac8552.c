/**
 * @file dac8552.c
 * @brief TI DAC8552 driver -- implementation.
 *
 * @author Orion Serup <orion@crablabs.io>
 */

#include "dac8552.h"

#include <stddef.h>

/* -- Control-byte field masks ------------------------------------------- *
 * Bit positions match SBAS362F table 1; ::DAC8552LoadMode and             *
 * ::DAC8552PowerMode enum values are already pre-shifted into the byte    *
 * so they can be OR-ed directly. Only the channel selector still needs    *
 * a runtime shift, hence the BUF_SEL_B mask below.                        */

#define DAC8552_CTRL_BUF_SEL_B 0x10U  ///< Bit 4 of control byte = 1 -> buffer B.

/* -- Static prototypes -------------------------------------------------- */

static DAC8552Error dac8552EmitFrame(const DAC8552* const dev,
                                     const uint8_t ctrl, const uint16_t code);

/* -- Public defs -------------------------------------------------------- */

DAC8552Error dac8552Init(DAC8552* const dev, const DAC8552HAL* const hal)
{
	if (dev == NULL || hal == NULL || hal->spiWrite == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	dev->hal = *hal;
	dev->is_initialized = true;
	return DAC8552_ERROR_OK;
}

DAC8552Error dac8552WriteCode(const DAC8552* const dev,
                              const DAC8552Channel ch,
                              const DAC8552LoadMode load_mode,
                              const DAC8552PowerMode power_mode,
                              const uint16_t code)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (ch != DAC8552_CHANNEL_A && ch != DAC8552_CHANNEL_B)
		return DAC8552_ERROR_INVALID_PARAM;

	const uint8_t buf_sel = (ch == DAC8552_CHANNEL_B) ? DAC8552_CTRL_BUF_SEL_B : 0U;
	const uint8_t ctrl = (uint8_t)load_mode | buf_sel | (uint8_t)power_mode;
	return dac8552EmitFrame(dev, ctrl, code);
}

DAC8552Error dac8552WriteBothSync(const DAC8552* const dev,
                                  const uint16_t code_a,
                                  const uint16_t code_b)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;

	// Stage A in its input buffer with no DAC update.
	const uint8_t ctrl_a = (uint8_t)DAC8552_LOAD_BUFFER_ONLY
	                     | 0U   // buf A
	                     | (uint8_t)DAC8552_POWER_ACTIVE;
	const DAC8552Error err_a = dac8552EmitFrame(dev, ctrl_a, code_a);
	if (err_a != DAC8552_ERROR_OK)
		return err_a;

	// Write B with LOAD_ALL: both outputs jump in lock-step on the
	// rising ~SYNC at the end of this frame.
	const uint8_t ctrl_b = (uint8_t)DAC8552_LOAD_ALL
	                     | DAC8552_CTRL_BUF_SEL_B
	                     | (uint8_t)DAC8552_POWER_ACTIVE;
	return dac8552EmitFrame(dev, ctrl_b, code_b);
}

/* -- Static defs -------------------------------------------------------- */

static DAC8552Error dac8552EmitFrame(const DAC8552* const dev,
                                     const uint8_t ctrl, const uint16_t code)
{
	const uint8_t frame[3] =
	{
		ctrl,
		(uint8_t)((code >> 8) & 0xFFU),
		(uint8_t)(code & 0xFFU),
	};
	if (dev->hal.spiWrite(frame, (uint8_t)sizeof(frame)) != 0)
		return DAC8552_ERROR_SPI;
	return DAC8552_ERROR_OK;
}
