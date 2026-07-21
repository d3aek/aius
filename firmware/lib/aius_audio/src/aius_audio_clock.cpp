/* Minimal PJRC Audio-library clock helper required by output_spdif3.cpp.
 * The full upstream utility is retained in the PlatformIO framework and its
 * implementation is reproduced here only to keep the selected S/PDIF source
 * units buildable without pulling the entire Audio library. */
#include <Arduino.h>
#include <utility/imxrt_hw.h>

#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
FLASHMEM
void set_audioClock(int nfact, int32_t nmult, uint32_t ndiv, bool force)
{
	if (!force && (CCM_ANALOG_PLL_AUDIO & CCM_ANALOG_PLL_AUDIO_ENABLE))
		return;
	CCM_ANALOG_PLL_AUDIO = CCM_ANALOG_PLL_AUDIO_BYPASS |
	                       CCM_ANALOG_PLL_AUDIO_ENABLE |
	                       CCM_ANALOG_PLL_AUDIO_POST_DIV_SELECT(2) |
	                       CCM_ANALOG_PLL_AUDIO_DIV_SELECT(nfact);
	CCM_ANALOG_PLL_AUDIO_NUM = nmult & CCM_ANALOG_PLL_AUDIO_NUM_MASK;
	CCM_ANALOG_PLL_AUDIO_DENOM = ndiv & CCM_ANALOG_PLL_AUDIO_DENOM_MASK;
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_POWERDOWN;
	while (!(CCM_ANALOG_PLL_AUDIO & CCM_ANALOG_PLL_AUDIO_LOCK)) {}
	CCM_ANALOG_MISC2 &= ~(CCM_ANALOG_MISC2_DIV_MSB | CCM_ANALOG_MISC2_DIV_LSB);
	CCM_ANALOG_PLL_AUDIO &= ~CCM_ANALOG_PLL_AUDIO_BYPASS;
}
#endif
