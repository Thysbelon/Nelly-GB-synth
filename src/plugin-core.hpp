#pragma once

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <vector>
#include <utility>
#include "gb.h"
#include "gb_struct_def.h"
#include "apu.h"
#include "timing.h"

#define GB_CLOCK_RATE 0x400000 // cycles per second
#define MAX_WAVES 0x3FFF
struct GameBoyPluginCore { // The part of the plugin that is standard agnostic
	GB_gameboy_t gb;
	double sampleRate;
	uint8_t NOISE_PITCH_LIST[127];
	uint8_t songWaveArray[MAX_WAVES][16]; // all wave data to be used by the song should be stored in a sysex message at the beginning. During the `run` method, if a sysex message is found, the plugin will take the sysex message, parse it as an array of wavetables, and store the result in this songWaveArray variable. Every time a CC21 message is detected, the plugin will use songWaveArray to write the correct wave to the APU. NOTE: the max number of waves is bottlenecked by CC21 which sets the index of the current wave to use; a CC can only go from 0-127, so there can be no more than 127 waves (and, even with garbage wave data, it is unlikely that a single song would have that many waves). TODO: if a song uses a musical sample (e.g. Pokemon Yellow samples pikachu voice clips. Music might sample drum sounds), is a max of 127 waves still enough? if not, I can always use two CC to create a 14-bit wave index selector.
	// to save space in memory, waves will be stored in the same format as gb: 32 samples long, with two 4-bit samples stored in each byte. However, the sysex message should store each 4-bit sample in its own byte, or else a wave containing the samples 0x0F and 0x07 right next to each other will be confused for the sysex end byte 0xF7.
	uint16_t curWaveIndex; // initialize this to 0
	uint8_t curWaveIndexLSB;
	uint8_t curWaveIndexMSB;
	bool legatoState[4 /* gb channels */]; // stores legatoState for each channel. When legatoState is true, new midi notes will only change pitch without retriggering the note.
	bool disableNoteOff[4];
	uint8_t userVol[4]; // DAW users expect that if they set a CC07 vol, it will remain until the next CC07 vol. This plugin has to overwrite the vol setting in the APU to handle noteOffs, so the user's set vol will be saved here. Every time a noteOn happens, if the vol in the APU is different than the user's set vol, re-write the user's set vol to the APU. userVol store volume in GB format, not midi format.
	uint8_t userEnvLen[4];
	uint8_t userEnvDirec[4];
	uint8_t userSoundLen[4];
	uint8_t lastMidiNote[4];
	uint16_t lastMidiPitchBend[4]; // 14-bit value
};

// helper functions of gb plugin
void resetInternalState(GameBoyPluginCore* self, double rate, bool isInstantiate = false /*only used by lv2 currently*/);

void setUpNoisePitchList(GameBoyPluginCore* self);
// gb helper functions end

struct midiMessage { // the code for specific plugin standards should convert their midi format to this generic midi format
	uint8_t statusByte;
	std::vector<uint8_t> dataBytes;
};

// process function. This is run for each frame in the current audio block. Hopefully this works with most plugin standards
std::pair<float, float> processFrame(GameBoyPluginCore* self, std::vector<midiMessage>& curFrameMidiEvs);
