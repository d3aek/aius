#ifndef AIUS_INPUT_SPDIF3_H
#define AIUS_INPUT_SPDIF3_H

#include <Arduino.h>
#include <AudioStream.h>
#include <DMAChannel.h>

class AudioInputSPDIF3 : public AudioStream
{
public:
	AudioInputSPDIF3(void) : AudioStream(0, NULL) { begin(); }
	virtual void update(void);
	void begin(void);
	static bool pllLocked(void);
	static unsigned int sampleRate(void);
protected:
	static bool update_responsibility;
	static DMAChannel dma;
	static void isr(void);
	/* Protected, not private as upstream: the receive hook in
	 * lib/aius_audio/src/aius_spdif_sources.cpp stages into this pair from its
	 * own DMA handler (aius_spdif_rx.h). */
	static audio_block_t *block_left;
	static audio_block_t *block_right;
	static uint16_t block_offset;
};

#endif
