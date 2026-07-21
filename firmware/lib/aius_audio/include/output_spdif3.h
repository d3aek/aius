#ifndef AIUS_OUTPUT_SPDIF3_H
#define AIUS_OUTPUT_SPDIF3_H

#include <Arduino.h>
#include <AudioStream.h>
#include <DMAChannel.h>

class AudioOutputSPDIF3 : public AudioStream
{
public:
	AudioOutputSPDIF3(void) : AudioStream(2, inputQueueArray) { begin(); }
	virtual void update(void);
	void begin(void);
	friend class AudioInputSPDIF3;
	static void mute_PCM(bool mute);
	static bool pll_locked(void);
protected:
	static void config_spdif3(void);
	static audio_block_t *block_left_1st;
	static audio_block_t *block_right_1st;
	static bool update_responsibility;
	static DMAChannel dma;
	static void isr(void);
private:
	static uint32_t dpll_Gain(void) __attribute__((const));
	static audio_block_t *block_left_2nd;
	static audio_block_t *block_right_2nd;
	static audio_block_t block_silent;
	audio_block_t *inputQueueArray[2];
};

#endif
