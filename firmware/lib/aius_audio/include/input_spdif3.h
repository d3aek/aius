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
private:
	static audio_block_t *block_left;
	static audio_block_t *block_right;
	static uint16_t block_offset;
};

#endif
