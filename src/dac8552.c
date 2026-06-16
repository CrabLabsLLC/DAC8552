/**
 * @file dac8552.c
 * @brief TI DAC8552 driver -- implementation. See dac8552.h for the API
 *        layering rationale: Init -> Configure -> Write / Latch family.
 *
 * @author Orion Serup <orion@crablabs.io>
 */

#include "dac8552.h"

#include <stddef.h>

/* -- Control-byte field masks ------------------------------------------ *
 * Bit positions match SLAS430A Figure 43 + Table 1.                       *
 *   DB23, DB22 : RESERVED (must be 0)                                     *
 *   DB21       : LDB  -- 1 = load DAC B from buffer B                     *
 *   DB20       : LDA  -- 1 = load DAC A from buffer A                     *
 *   DB19       : don't care                                               *
 *   DB18       : Buffer Select (0 = write data to buffer A,               *
 *                               1 = write data to buffer B)               *
 *   DB17, DB16 : PD1, PD0 (power mode of the buffer being written)        *
 *                                                                         *
 * ::DAC8552LoadMode and ::DAC8552PowerMode enum values are pre-shifted    *
 * into the byte so they can be OR-ed directly. The channel selector       *
 * needs a runtime shift, hence the BUF_SEL_B mask below.                  */

#define DAC8552_CTRL_BUF_SEL_B 0x04U  ///< Bit 2 of control byte = 1 -> data goes to buffer B.

/* -- Static prototypes -------------------------------------------------- */

static DAC8552Error dac8552EmitFrame(const DAC8552* const dev,
                                     const uint8_t ctrl, const uint16_t code);
static uint8_t dac8552BuildCtrl(const DAC8552Channel ch,
                                const DAC8552LoadMode load_mode,
                                const DAC8552PowerMode power_mode);
static bool dac8552IsValidChannel(const DAC8552Channel ch);

/* -- 1. Lifecycle ------------------------------------------------------- */

DAC8552Error dac8552Init(DAC8552* const dev, const DAC8552HAL* const hal)
{
	if (dev == NULL || hal == NULL || hal->spiWrite == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	dev->hal = *hal;
	dev->is_initialized = true;
	// Chip boots both channels in Hi-Z per SBAS362F; reflect that in the
	// driver cache so ::dac8552Configure sees an accurate prior state.
	for (uint8_t i = 0U; i < DAC8552_CHANNEL_COUNT; ++i)
	{
		dev->power_mode[i] = DAC8552_POWER_HIZ;
		dev->last_value[i] = 0U;
	}
	return DAC8552_ERROR_OK;
}

/* -- 2. Power-mode control --------------------------------------------- */

DAC8552Error dac8552Configure(DAC8552* const dev,
                              const DAC8552PowerMode power_a,
                              const DAC8552PowerMode power_b)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;

	// Stage channel A: write 0 to buffer A with A's PD field; no DAC
	// update yet (LDA = LDB = 0).
	const uint8_t ctrl_a = dac8552BuildCtrl(DAC8552_CHANNEL_A,
	                                        DAC8552_LOAD_BUFFER_ONLY,
	                                        power_a);
	const DAC8552Error err_a = dac8552EmitFrame(dev, ctrl_a, 0U);
	if (err_a != DAC8552_ERROR_OK)
		return err_a;

	// Channel B with LOAD_ALL (LDA=1, LDB=1): write 0 to buffer B with
	// B's PD field, AND simultaneously latch BOTH DAC registers from
	// their buffers (both are now 0).
	const uint8_t ctrl_b = dac8552BuildCtrl(DAC8552_CHANNEL_B,
	                                        DAC8552_LOAD_ALL,
	                                        power_b);
	const DAC8552Error err_b = dac8552EmitFrame(dev, ctrl_b, 0U);
	if (err_b != DAC8552_ERROR_OK)
		return err_b;

	dev->power_mode[DAC8552_CHANNEL_A] = power_a;
	dev->power_mode[DAC8552_CHANNEL_B] = power_b;
	dev->last_value[DAC8552_CHANNEL_A] = 0U;
	dev->last_value[DAC8552_CHANNEL_B] = 0U;
	return DAC8552_ERROR_OK;
}

DAC8552Error dac8552SetPowerMode(DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 const DAC8552PowerMode mode)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;

	// Buffer-only frame: PD field captures for @p ch; the buffer is
	// re-written with its current cached code so the next latch keeps
	// the same output voltage.
	const uint8_t ctrl = dac8552BuildCtrl(ch, DAC8552_LOAD_BUFFER_ONLY, mode);
	const DAC8552Error err = dac8552EmitFrame(dev, ctrl, dev->last_value[ch]);
	if (err != DAC8552_ERROR_OK)
		return err;
	dev->power_mode[ch] = mode;
	return DAC8552_ERROR_OK;
}

/* -- 3. Buffer staging -------------------------------------------------- */

DAC8552Error dac8552WriteBuffer(DAC8552* const dev,
                                const DAC8552Channel ch,
                                const uint16_t code)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;

	const uint8_t ctrl = dac8552BuildCtrl(ch, DAC8552_LOAD_BUFFER_ONLY,
	                                      dev->power_mode[ch]);
	const DAC8552Error err = dac8552EmitFrame(dev, ctrl, code);
	if (err != DAC8552_ERROR_OK)
		return err;
	dev->last_value[ch] = code;
	return DAC8552_ERROR_OK;
}

/* -- 4. Output latch --------------------------------------------------- */

DAC8552Error dac8552Write(DAC8552* const dev,
                          const DAC8552Channel ch,
                          const uint16_t code)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;

	// Per SLAS430A Operation Example 2: "Load New Data to DAC A and
	// DAC B Sequentially". One frame writes data to the selected
	// channel's input buffer AND latches that channel's DAC register.
	// Channel A uses LDA; channel B uses LDB. Buffer Select must point
	// at the same channel so the 16-bit data lands in the right buffer.
	const DAC8552LoadMode load = (ch == DAC8552_CHANNEL_A)
	                             ? DAC8552_LOAD_A : DAC8552_LOAD_B;
	const uint8_t ctrl = dac8552BuildCtrl(ch, load, dev->power_mode[ch]);
	const DAC8552Error err = dac8552EmitFrame(dev, ctrl, code);
	if (err != DAC8552_ERROR_OK)
		return err;
	dev->last_value[ch] = code;
	return DAC8552_ERROR_OK;
}

DAC8552Error dac8552WriteAll(DAC8552* const dev,
                             const uint16_t code_a,
                             const uint16_t code_b)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;

	// Stage A in its input buffer with no DAC update.
	const uint8_t ctrl_a = dac8552BuildCtrl(DAC8552_CHANNEL_A,
	                                        DAC8552_LOAD_BUFFER_ONLY,
	                                        dev->power_mode[DAC8552_CHANNEL_A]);
	const DAC8552Error err_a = dac8552EmitFrame(dev, ctrl_a, code_a);
	if (err_a != DAC8552_ERROR_OK)
		return err_a;

	// Write B with LOAD_ALL: both outputs jump in lock-step on the
	// rising ~SYNC at the end of this frame.
	const uint8_t ctrl_b = dac8552BuildCtrl(DAC8552_CHANNEL_B,
	                                        DAC8552_LOAD_ALL,
	                                        dev->power_mode[DAC8552_CHANNEL_B]);
	const DAC8552Error err_b = dac8552EmitFrame(dev, ctrl_b, code_b);
	if (err_b != DAC8552_ERROR_OK)
		return err_b;

	dev->last_value[DAC8552_CHANNEL_A] = code_a;
	dev->last_value[DAC8552_CHANNEL_B] = code_b;
	return DAC8552_ERROR_OK;
}

DAC8552Error dac8552LatchChannel(DAC8552* const dev, const DAC8552Channel ch)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;

	// Latch the channel's DAC register from its current input buffer.
	// We re-emit a frame with the cached code -- the buffer write is a
	// no-op (same code), but LDA/LDB asserts so the DAC register reloads.
	const DAC8552LoadMode load = (ch == DAC8552_CHANNEL_A)
	                             ? DAC8552_LOAD_A : DAC8552_LOAD_B;
	const uint8_t ctrl = dac8552BuildCtrl(ch, load, dev->power_mode[ch]);
	return dac8552EmitFrame(dev, ctrl, dev->last_value[ch]);
}

DAC8552Error dac8552LatchAll(DAC8552* const dev)
{
	if (dev == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;

	// Re-emit both cached values via a synchronous LOAD_ALL pair.
	// Neither input buffer's value changes, but both DAC registers
	// update simultaneously from the buffers on the second frame's
	// rising ~SYNC.
	const uint8_t ctrl_a = dac8552BuildCtrl(DAC8552_CHANNEL_A,
	                                        DAC8552_LOAD_BUFFER_ONLY,
	                                        dev->power_mode[DAC8552_CHANNEL_A]);
	const DAC8552Error err_a = dac8552EmitFrame(dev, ctrl_a,
	                                            dev->last_value[DAC8552_CHANNEL_A]);
	if (err_a != DAC8552_ERROR_OK)
		return err_a;

	const uint8_t ctrl_b = dac8552BuildCtrl(DAC8552_CHANNEL_B,
	                                        DAC8552_LOAD_ALL,
	                                        dev->power_mode[DAC8552_CHANNEL_B]);
	return dac8552EmitFrame(dev, ctrl_b,
	                        dev->last_value[DAC8552_CHANNEL_B]);
}

/* -- 5. Read-back (driver-side cache) ---------------------------------- */

DAC8552Error dac8552GetLastValue(const DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 uint16_t* const out_code)
{
	if (dev == NULL || out_code == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;
	*out_code = dev->last_value[ch];
	return DAC8552_ERROR_OK;
}

DAC8552Error dac8552GetPowerMode(const DAC8552* const dev,
                                 const DAC8552Channel ch,
                                 DAC8552PowerMode* const out_mode)
{
	if (dev == NULL || out_mode == NULL)
		return DAC8552_ERROR_NULL_PARAM;
	if (!dev->is_initialized)
		return DAC8552_ERROR_NOT_INITIALIZED;
	if (!dac8552IsValidChannel(ch))
		return DAC8552_ERROR_INVALID_PARAM;
	*out_mode = dev->power_mode[ch];
	return DAC8552_ERROR_OK;
}

/* -- Static defs -------------------------------------------------------- */

static bool dac8552IsValidChannel(const DAC8552Channel ch)
{
	return (ch == DAC8552_CHANNEL_A) || (ch == DAC8552_CHANNEL_B);
}

static uint8_t dac8552BuildCtrl(const DAC8552Channel ch,
                                const DAC8552LoadMode load_mode,
                                const DAC8552PowerMode power_mode)
{
	const uint8_t buf_sel = (ch == DAC8552_CHANNEL_B) ? DAC8552_CTRL_BUF_SEL_B : 0U;
	return (uint8_t)load_mode | buf_sel | (uint8_t)power_mode;
}

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
