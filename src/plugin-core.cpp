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
#include "plugin-core.hpp"

// helper functions of gb plugin
void resetInternalState(GameBoyPluginCore* self, double rate, bool isInstantiate){
	memset(&(self->gb),0,sizeof(GB_gameboy_t));
	if (isInstantiate==true) {
		self->gb.model = GB_MODEL_DMG_B; // default model.
	}
	GB_apu_init(&(self->gb));
	if (isInstantiate==true) self->gb.model = GB_MODEL_DMG_B;
	if (rate) {
		printf("DAW sample rate: %lf\n", rate);
		self->sampleRate=rate;
		GB_set_sample_rate(&(self->gb),(unsigned)(int)round(rate));
	} else if (self->sampleRate) {
		GB_set_sample_rate(&(self->gb),(unsigned)(int)round(self->sampleRate));
	} else {
		printf("Warning: GB sample rate not set!\n");
	}
	GB_set_highpass_filter_mode(&(self->gb), GB_HIGHPASS_ACCURATE); // the default mode is GB_HIGHPASS_OFF
	GB_apu_write(&(self->gb), GB_IO_NR10, 0); // disable square 1 pitch sweep.
	GB_apu_write(&(self->gb), GB_IO_NR52, 0x8f); // Power on APU. writing to bits 3-0 of this register *shouldn't* do anything because those bits are read only, but some emulators require them to be written to in order to enable channels.
	GB_apu_write(&(self->gb), GB_IO_NR51, 0xFF); // Enable all channels and set panning to center.
	GB_apu_write(&(self->gb), GB_IO_NR50, 0x77); // set master volume to max.
	
	//set env. Set volume to max, set envelope direction to down (decrease volume), and set envelope length to 0 (disables envelope).
	GB_apu_write(&(self->gb), GB_IO_NR12, 0xF0);
	GB_apu_write(&(self->gb), GB_IO_NR22, 0xF0);
	GB_apu_write(&(self->gb), GB_IO_NR32, 0b01 << 5); // ?
	GB_apu_write(&(self->gb), GB_IO_NR42, 0xF0);
	// trigger channel
	GB_apu_write(&(self->gb), GB_IO_NR14, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR24, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR34, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR44, 0x80);
		
	GB_advance_cycles(&(self->gb), 0xFF); // TODO: reduce this cycle number to improve performance slightly.
		
	// trigger channel again with 0 vol. This shouldn't mess up the user playing notes, because this is the same state as after a note off.
	// Envelope settings are: Volume 0, envelope direction up, and envelope length 0.
	// Setting all envelope settings except envelope direction to 0 is the best way to mute a channel without powering off the channel's DAC (powering off the channel's DAC would cause a pop sound).
	GB_apu_write(&(self->gb), GB_IO_NR12, 8);
	GB_apu_write(&(self->gb), GB_IO_NR22, 8);
	GB_apu_write(&(self->gb), GB_IO_NR32, 0);
	GB_apu_write(&(self->gb), GB_IO_NR42, 8);
	GB_apu_write(&(self->gb), GB_IO_NR14, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR24, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR34, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR44, 0x80);
	
	for (int i=0; i<0xFFFF; i++){
		GB_advance_cycles(&(self->gb), 0xFF);
		if (self->gb.apu_output.final_sample.left == 0) break;
		if (i==0xFFFF-1) printf("loop never broke\n");
	}
	// advance past APU pop
	
	// I and users should avoid anything that turns the channel off. It will cause the next note played to be too loud
	
	for (int i=0; i<4; i++){
		self->userVol[i]=0x0F; // GB format!
		self->userEnvLen[i]=0; 
		self->userEnvDirec[i]=0; // an env direc of "up" and an env length of 0 leads to volume weirdness.
		self->userSoundLen[i]=0;
		self->lastMidiNote[i]=0xFF; // C4
		self->lastMidiPitchBend[i]=0x2000; // center.
	}
	self->userVol[2]=1;
	self->curWaveIndex = 0;
	self->curWaveIndexLSB = 0;
	self->curWaveIndexMSB = 0;
	
	if (isInstantiate==true) {
		for (uint16_t i=0; i<MAX_WAVES; i++){ // if the wave sysex message is only sent at the start of the song, then the wave channel might break if the user pauses and resumes in the middle of a song, which would be bad. Maybe I can get away with not resetting wave stuff during activate()? NOTE: I think I can make wave stuff persist if I turn it into a user-facing control rather than an internal state.
			for (uint8_t i2=0; i2<16; i2++){
				self->songWaveArray[i][i2]=0;
			}
		}
	}
	
}

static uint16_t midiNoteAndPitchBend2gbPitch(uint8_t midiNote, uint16_t midiPitchBend, uint8_t channel, uint8_t NOISE_PITCH_LIST[]){
	uint8_t const noteC2=36; // midi note number
	if (channel!=3) {
		uint16_t gbPitchArray[] = {44,156,262,363,457,547,631,710,786,854,923,986,1046,1102,1155,1205,1253,1297,1339,1379,1417,1452,1486,1517,1546,1575,1602,1627,1650,1673,1694,1714,1732,1750,1767,1783,1798,1812,1825,1837,1849,1860,1871,1881,1890,1899,1907,1915,1923,1930,1936,1943,1949,1954,1959,1964,1969,1974,1978,1982,1985,1988,1992,1995,1998,2001,2004,2006,2009,2011,2013,2015}; // length: 72
		// first entry in gbPitchArray is C2
		int8_t gbPitchArrNoteI = midiNote - noteC2;
		if (gbPitchArrNoteI < 0) {
			gbPitchArrNoteI=0;
		} else if (gbPitchArrNoteI > 71) {
			gbPitchArrNoteI=71;
		}
		uint16_t gbPitchNote = gbPitchArray[gbPitchArrNoteI];
	#define MIDI_PITCH_CENTER 0x2000
		if (midiPitchBend == MIDI_PITCH_CENTER) {
			return gbPitchNote;
		} else {
			const int pitchBendRange = 2; // TODO: have this be set by RPN
			float bendSemitones = ((float)midiPitchBend - (float)MIDI_PITCH_CENTER) * ((float)pitchBendRange / MIDI_PITCH_CENTER); // if pitchBendRange is 2, this will be a number between -2 and +2.
			if (midiPitchBend > MIDI_PITCH_CENTER) {
				int intBendSemitones = (int)ceil(bendSemitones);
				if (gbPitchArrNoteI + intBendSemitones > 71) {
					return gbPitchArray[71];
				} else if (intBendSemitones == bendSemitones) {
					return gbPitchArray[gbPitchArrNoteI + intBendSemitones];
				} else {
					int intFlooredBendSemitones = (int)floor(bendSemitones);
					int gbPitchDiff = gbPitchArray[gbPitchArrNoteI + intBendSemitones] - gbPitchArray[gbPitchArrNoteI + intFlooredBendSemitones];
					return (uint16_t)round(gbPitchArray[gbPitchArrNoteI + intFlooredBendSemitones] + ((float)gbPitchDiff * (bendSemitones - intFlooredBendSemitones)));
				}
			} else { // midiPitchBend < MIDI_PITCH_CENTER
				int intBendSemitones = (int)floor(bendSemitones);
				if (gbPitchArrNoteI + intBendSemitones < 0) {
					return gbPitchArray[0];
				} else if (intBendSemitones == bendSemitones) {
					return gbPitchArray[gbPitchArrNoteI + intBendSemitones];
				} else {
					int intCeiledBendSemitones = (int)ceil(bendSemitones);
					int gbPitchDiff = gbPitchArray[gbPitchArrNoteI + intCeiledBendSemitones] - gbPitchArray[gbPitchArrNoteI + intBendSemitones];
					return (uint16_t)round(gbPitchArray[gbPitchArrNoteI + intCeiledBendSemitones] - ((float)gbPitchDiff * (fabsf(bendSemitones) - fabsf((float)intCeiledBendSemitones))));
				}
			}
		}
	} else {
		// handle noise
		// noise pitch spans the whole midi note range.
		int8_t gbPitchArrNoteI = midiNote;
		if (gbPitchArrNoteI > 126) { // NOISE_PITCH_LIST has a size of 127. 126 is the last index
			gbPitchArrNoteI=126;
		}
		return (uint16_t)NOISE_PITCH_LIST[gbPitchArrNoteI];
	}
}

static void writeNewPitchToAPU(GB_gameboy_t* gbPointer, uint16_t newPitch, uint8_t channel, bool isTrigger, uint8_t soundLenEn){
	if (channel!=3) {
		uint8_t regVal = 0;
		if (soundLenEn < 2) {
			regVal |= soundLenEn << 6;
		} else {
			regVal = GB_apu_read(gbPointer, GB_IO_NR14 + channel*5) & 0b01000000; // preserve length enable
			// NOTE: GB_apu_read can return garbage bits from areas that are write-only! Remember to always zero-out bits that are not needed.
		}
		regVal |= (uint8_t)((newPitch & 0b0000011100000000)>>8);
		if (isTrigger) regVal |= 0b10000000;
		GB_apu_write(gbPointer, GB_IO_NR14 + channel*5, regVal);
		regVal = newPitch & 0xFF;
		GB_apu_write(gbPointer, GB_IO_NR13 + channel*5, regVal);
	} else {
		// handle noise
		uint8_t regVal = GB_apu_read(gbPointer, GB_IO_NR44) & 0b01000000; // preserve length enable
		if (isTrigger) regVal |= 0b10000000; // NOTE: for the noise channel, changing the pitch without retriggering the channel does nothing.
		GB_apu_write(gbPointer, GB_IO_NR44, regVal);
		regVal = GB_apu_read(gbPointer, GB_IO_NR43) & 0b00001000; // preserve noise width
		regVal |= ((uint8_t)newPitch & 0b11110111);
		GB_apu_write(gbPointer, GB_IO_NR43, regVal);
	}
}

static uint8_t convertMidiValToRange(uint8_t inMidiVal, uint8_t outValMax){ // used to convert midi vals to register bit val range.
	const uint8_t MIDI_CC_MAX = 0x7F;
	return (uint8_t)round((float)outValMax * ((float)inMidiVal / MIDI_CC_MAX));
}

void setUpNoisePitchList(GameBoyPluginCore* self){
	// set up NOISE_PITCH_LIST
	uint8_t noisePitchListSize=0;
	for (int16_t noisePitch=0xF7; noisePitch >= 0; noisePitch--){ // the list should be written backwards because lower values tend to be higher pitched. 0xF7 is 0b11110111.
		if ((noisePitch & 8) == 0) {
			self->NOISE_PITCH_LIST[noisePitchListSize]=(uint8_t)noisePitch;
			noisePitchListSize++;
		}
	}
}

// gb helper functions end

// process function. This is run for each frame in the current audio block. Hopefully this works with most plugin standards
std::pair<float, float> processFrame(GameBoyPluginCore* self, std::vector<midiMessage>& curFrameMidiEvs){ // TODO: return a pair of float samples
	// these boolean arrays exist to make sure that simultaneous events don't accidently overwrite each other.
	bool noteOn[4]={false, false, false, false}; // a note on was sent at this position
	bool noteTriggered[4]={false, false, false, false};
	bool cc21set=false;
	bool cc53set=false;
	for (uint32_t evI=0; evI<curFrameMidiEvs.size(); evI++) {
		//const clap_event_header_t *event = curFrameMidiEvs[evI];
		uint8_t midiMessageType = curFrameMidiEvs[evI].statusByte & 0xF0; // the 4 least significant bits of the status byte contain the channel. Discard them to get just the midi event type
		
		uint8_t msg[3]; // The midi message has a variable length. The first byte is always the status byte.
		msg[0] = curFrameMidiEvs[evI].statusByte;
		for (int i=1; i<3; i++){
			if (i-1 >= curFrameMidiEvs[evI].dataBytes.size()){
				msg[i] = 0;
			} else {
				msg[i] = curFrameMidiEvs[evI].dataBytes[i-1];
			}
		}
		uint8_t channel=0xFF;
		channel = (curFrameMidiEvs[evI].statusByte) & 0x0F; // https://michd.me/jottings/midi-message-format-reference/
		if (channel > 3) channel=0; // NOTE: this channel value shouldn't be used for midi events that are channel-agnostic
		// TODO: check if APU exists before running APU methods
		// convert midi event to gb apu register write
		uint16_t newPitch=0;
		bool isTrigger=false;
		uint8_t regVal=0;
		
		switch (midiMessageType) {
			case 0xF0: // SYSEX
			{
				printf("sysex message received. Collecting waves...\n");
				
				const uint32_t sysexSize = curFrameMidiEvs[evI].dataBytes.size();
				printf("sysexSize: %u\n", sysexSize);
				if (sysexSize < 32) {
					printf("Appears to be a garbage sysex. Ignoring...\n");
				} else {
					std::vector<uint8_t>& sysexData = curFrameMidiEvs[evI].dataBytes;
				
					for (uint16_t i=0; i<MAX_WAVES; i++){ // If I'm not resetting wave data during activate(), I need to reset it when a sysex message is received.
						for (uint8_t i2=0; i2<16; i2++){
							self->songWaveArray[i][i2]=0;
						}
					}
					
					bool breakImmediately=false; // I could use a goto instead, but this feels safer.
					for (uint16_t waveI=0; waveI<MAX_WAVES; waveI++) {
						printf("Wave %u:\n", waveI);
						for (uint8_t samplePairI=0; samplePairI<16; samplePairI++) {
							uint32_t samplePairFirstSysexI = ((uint32_t)waveI)*32+((uint32_t)samplePairI)*2; // index in the sysex data
							uint32_t samplePairSecondSysexI = samplePairFirstSysexI+1;
							if (sysexData[samplePairFirstSysexI]==0xF7 || sysexData[samplePairSecondSysexI]==0xF7 || samplePairSecondSysexI >= sysexSize) {breakImmediately=true; break;} // end of sysex
							self->songWaveArray[waveI][samplePairI] = (sysexData[samplePairFirstSysexI] << 4) | sysexData[samplePairSecondSysexI];
							printf("%02X ", self->songWaveArray[waveI][samplePairI]);
						}
						if (breakImmediately==true) break;
						printf("\n");
					}
					printf("\nend of sysex\n");
					//printf("sysexSize: %u\n", sysexSize);
				}
				
			}
				break;
				
			case 0xB0: // MIDI_MSG_CONTROLLER
				// msg[1] control number
				// msg[2] control value
				switch (msg[1]){
					case 7: // MIDI_CTL_MSB_MAIN_VOLUME
						regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
						regVal &= 0b00001111; // keep envDir and envLen
						switch (channel){
							case 2: // wave
								if (msg[2] >= 96) {
									self->userVol[channel] = 0b01;
								} else if (msg[2] >= 48) {
									self->userVol[channel] = 0b10;
								} else if (msg[2] > 0) {
									self->userVol[channel] = 0b11;
								} else {
									// if ccVol is 0, set wav vol to 0
									self->userVol[channel] = 0;
								}
								regVal |= ((self->userVol[channel]) << 5);
								break;
							default:
								self->userVol[channel] = round((float)(float)(msg[2] / (float)0x7F) * (float)0x0F);
								regVal |= (self->userVol[channel]) << 4;
								break;
						}
						//printf("midi vol: %u, new userVol: %u\n", msg[2], self->userVol[channel]);
						GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
						break;
					case 9: /* gb pan mute (0 0) */
						if (msg[2] >= 64) {
							regVal=GB_apu_read(&(self->gb), GB_IO_NR51);
							regVal &= (uint8_t)((~(0x11 << channel)) & 0xFF); // discard only the bits currently being modified.
							GB_apu_write(&(self->gb), GB_IO_NR51, regVal);
						}
						break;
					case 10: // MIDI_CTL_MSB_PAN
						regVal=GB_apu_read(&(self->gb), GB_IO_NR51);
						regVal &= (uint8_t)((~(0x11 << channel)) & 0xFF); // discard only the bits currently being modified.
						if (msg[2] >= 96) {
							regVal |= (0x01 << channel);
						} else if (msg[2] >= 32) {
							regVal |= (0x11 << channel);
						} else { // if (msg[2] >= 0)
							regVal |= (0x10 << channel);
						}
						GB_apu_write(&(self->gb), GB_IO_NR51, regVal);
						break;
					case 12: /*cc12 envelope direction*/
						if (channel!=2) {
							regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
							regVal &= 0b11110111; // keep env start vol and envLen
							if (msg[2] >= 64) {
								self->userEnvDirec[channel]=1;
							} else {
								self->userEnvDirec[channel]=0;
							}
							regVal |= (self->userEnvDirec[channel] << 3);
							GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
						}
						break;
					case 13: /*cc13 envelope length*/
						if (channel!=2) {
							regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
							regVal &= 0b11111000; // keep env start vol and envDir
							self->userEnvLen[channel] = (uint8_t)round(((float)msg[2] / 0x7F) * 7) & 0b00000111;
							regVal |= self->userEnvLen[channel];
							GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
						}
						break;
					// TODO: find a gbs to TEST sound length settings.
					// TODO: BUG: CGB-BPJE-USA.gbs subsong 2 shows that sweep is not functioning correctly. Downward pitch sweeps are used to simulate the sound of a tom drum or snare, but Nelly's output is too high pitched (I've been using gbsplay as "correct output"). There do not appear to be any bugs in gbs2midi's or Nelly's conversion. It is completely unknown what is causing the bug. *Help is needed to fix pitch sweep*.
					case 14: /*sound length enable*/
					{
						uint8_t soundLenEn = msg[2] >= 64 ? 1 : 0;
						newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
						writeNewPitchToAPU(&(self->gb), newPitch, channel, noteTriggered[channel], soundLenEn);
					}
						break;
					case 15: /*sound length*/ // TODO: verify that the correct value is being written to the GB APU register; it sounds a bit short.
					{
						uint8_t soundLen = convertMidiValToRange(msg[2], channel == 2 ? 0xFF : 0x3F);
						if (channel == 2) {
							GB_apu_write(&(self->gb), GB_IO_NR11 + channel*5, soundLen);
						} else {
							regVal = GB_apu_read(&(self->gb), GB_IO_NR11 + channel*5);
							regVal &= 0b11000000; // preserve duty cycle.
							regVal |= (soundLen & 0b00111111);
							GB_apu_write(&(self->gb), GB_IO_NR11 + channel*5, regVal);
						}
						self->userSoundLen[channel] = soundLen;
					}
						break;
					case 16: // sweep speed
						if (channel == 0){
							uint8_t sweepSpeed = convertMidiValToRange(msg[2], 7);
							regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
							regVal &= 0b00001111;
							regVal |= ((sweepSpeed & 0b111) << 4);
							GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
						}
						break;
					case 17: // sweep shift
						if (channel == 0){ // high period == high frequency == high pitch.
							uint8_t sweepShift = convertMidiValToRange(msg[2], 7);
							regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
							regVal &= 0b01111000; // zero out previous sweep shift, keep all other values.
							regVal |= (sweepShift & 0b111);
							GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
						}
						break;
					case 18: // sweep up or down. for readability, pitch should go up when cc18 is 127, and down when cc18 is 0
						if (channel == 0){
							uint8_t sweepDir = convertMidiValToRange(msg[2], 1);
							regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
							regVal &= 0b01110111;
							sweepDir ^= 1; // invert sweepDir before writing to register. In GB, 0 is "increase pitch", which is unintuitive. Inverting sweepDir before writing to the emulated GB allows the user to use a CC of 127 as "increase pitch", which is more intuitive.
							regVal |= ((sweepDir & 1) << 3);
							GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
						}
						break;
					case 19: // duty cycle A.K.A. pulse width
						if (channel <= 1) { // square channels
							uint8_t dutyCycleVal=0;
							dutyCycleVal = (uint8_t)round(((float)msg[2] / 0x7F) * 3);
							regVal=0;
							regVal |= (dutyCycleVal << 6);
							regVal |= (self->userSoundLen[channel] & 0b00111111); // sound length is write-only. Rewrite it so it isn't lost
							GB_apu_write(&(self->gb), GB_IO_NR11 + channel*5, regVal);
						}
						break;
					case 20: // noise long or short
						if (channel == 3) {
							regVal = GB_apu_read(&(self->gb), GB_IO_NR43); // I believe this is read/write, unlike the other pitch values
							regVal &= 0b11110111; // remove noise width, keep pitch values.
							uint8_t noiseWidth = msg[2] >= 64 ? 1 : 0;
							regVal |= (noiseWidth << 3);
							GB_apu_write(&(self->gb), GB_IO_NR43, regVal);
						}
						break;
					case 21: // wave index selector. TODO: although I'm now keeping wave data after activate, which makes it possible to skip through a song, it is still not possible to play wave notes in the piano roll while playback is paused. This is because I reset the GB APU when the user pauses playback so no notes play while paused. Although wave data is saved in the plugin's memory, that data is cleared from the GB APU, and CC21 can't be sent or received while playback is paused. Maybe, in the code for handling when playback is paused, I should use curWaveIndex to rewrite the wave data to the wave channel, so the user can use the piano roll?
						// in a hexadecimal representation of a number, the MSB is the leftmost byte, the LSB is the rightmost byte
						self->curWaveIndexMSB=msg[2];
						cc21set=true;
						if (cc53set){
							self->curWaveIndex = ((uint16_t)(self->curWaveIndexMSB) << 7) | self->curWaveIndexLSB;
							printf("self->curWaveIndex: %u\n", self->curWaveIndex);
							
							GB_apu_write(&(self->gb), GB_IO_NR30, 0); // turn off DAC
							GB_advance_cycles(&(self->gb), 1); // TODO: check if advancing cycles here can mess up other channels.
							for (uint8_t samplePairI=0; samplePairI<16; samplePairI++) { // write to wave ram
								GB_apu_write(&(self->gb), GB_IO_WAV_START+samplePairI, self->songWaveArray[self->curWaveIndex][samplePairI]);
							}
							GB_advance_cycles(&(self->gb), 1);
							GB_apu_write(&(self->gb), GB_IO_NR30, 0b10000000); // turn on DAC
							GB_advance_cycles(&(self->gb), 1);
							newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
							writeNewPitchToAPU(&(self->gb), newPitch, channel, true, 0xFF); // trigger channel
							noteTriggered[channel]=true;
						}
						// wave should ONLY be triggered when switching waves. Triggering it at any other time will unpredictably corrupt wave ram.
						// TODO: does wave need to be re-triggered to change the volume? My midi output suggests that it doesn't need to be re-triggered, but pandocs implies that it does: "Trigger (Write-only): Writing any value to NR34 with this bit set triggers the channel, causing the following to occur:.. ...Volume is set to contents of NR32 initial volume."
						break;
					case 53: // TODO: Reduce duplicate code.
						self->curWaveIndexLSB=msg[2];
						cc53set=true;
						if (cc21set){
							self->curWaveIndex = ((uint16_t)(self->curWaveIndexMSB) << 7) | self->curWaveIndexLSB;
							//printf("self->curWaveIndex: %u\n", self->curWaveIndex);
							
							GB_apu_write(&(self->gb), GB_IO_NR30, 0); // turn off DAC
							GB_advance_cycles(&(self->gb), 1); // TODO: check if advancing cycles here can mess up other channels.
							for (uint8_t samplePairI=0; samplePairI<16; samplePairI++) { // write to wave ram
								GB_apu_write(&(self->gb), GB_IO_WAV_START+samplePairI, self->songWaveArray[self->curWaveIndex][samplePairI]);
							}
							GB_advance_cycles(&(self->gb), 1);
							GB_apu_write(&(self->gb), GB_IO_NR30, 0b10000000); // turn on DAC
							GB_advance_cycles(&(self->gb), 1);
							newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
							writeNewPitchToAPU(&(self->gb), newPitch, channel, true, 0xFF); // trigger channel
							noteTriggered[channel]=true;
						}
						break;
					case 23:{ // change GB model via midi messages.
						GB_model_t chosenModel;
						switch (msg[2]){
							default:
							case 0:
								chosenModel = GB_MODEL_DMG_B;
								break;
							case 1:
								chosenModel = GB_MODEL_SGB_NTSC;
								break;
							case 2:
								chosenModel = GB_MODEL_SGB_PAL;
								break;
							case 3:
								chosenModel = GB_MODEL_SGB_NTSC_NO_SFC;
								break;
							case 4:
								chosenModel = GB_MODEL_SGB_PAL_NO_SFC;
								break;
							case 5:
								chosenModel = GB_MODEL_SGB2;
								break;
							case 6:
								chosenModel = GB_MODEL_SGB2_NO_SFC;
								break;
							case 7:
								chosenModel = GB_MODEL_CGB_C;
								break;
							case 8:
								chosenModel = GB_MODEL_CGB_E;
								break;
							case 9:
								chosenModel = GB_MODEL_AGB;
								break;
							case 10:
								chosenModel = GB_MODEL_AGB_NATIVE;
								break;
						}
						self->gb.model = chosenModel;
						resetInternalState(self, false, false);
						self->gb.model = chosenModel;
						break;
					}
					default:
						break;
				}
				break;
			case 0x80: // MIDI_MSG_NOTE_OFF
				if (noteOn[channel]==false) { // This noteOn variable only tracks if a noteOn has been sent at this exact time. If a Note On and a Note Off occur at the same time on the same channel, the Note On should take priority.
					if (channel!=2) {
						uint8_t tempReg = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
						uint8_t envDirec = tempReg & 0b00001000;
						uint8_t envLen = tempReg & 0b00000111;
						uint8_t curVol=0xFF; // intention: current volume as set by the envelope.
						switch(channel){
							case 0:
								curVol=self->gb.apu.square_channels[0].current_volume;
								break;
							case 1:
								curVol=self->gb.apu.square_channels[1].current_volume;
								break;
							case 3:
								curVol=self->gb.apu.noise_channel.current_volume;
								break;
							default:
								break;
						}
						if (GB_apu_read(&(self->gb), GB_IO_NR52) & (0b00000001 << channel) && !((curVol==0 && envDirec == 0/*down*/) || (curVol==0 && envLen==0)) && self->lastMidiNote[channel] == msg[1]) { // if channel is enabled AND the current volume is greater than 0. make sure a false positive doesn't happen when a channel starts at 0 vol then goes up via envelope. Do not silence the channel if the midi note that's currently ending is different from the most recent note-on; this makes it possible to clearly disable note-offs for specific notes by having the note ends trail and overlap each other.
							regVal = 0b00001000; // set envelope direction to "up" to silence the channel WITHOUT turning off the DAC (which could cause a pop)
							GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
							newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
							writeNewPitchToAPU(&(self->gb), newPitch, channel, true, 0xFF); // have to retrigger the channel for the silence to take effect.
							noteTriggered[channel]=true;
						}
					} else { // wave
						uint8_t curVol=GB_apu_read(&(self->gb), GB_IO_NR32) & 0b01100000; // exact number doesn't matter, I'm just checking if this is zero or not
						if (GB_apu_read(&(self->gb), GB_IO_NR52) & 0b00000100 && curVol > 0) { // if channel is enabled AND the current volume is greater than 0.
							GB_apu_write(&(self->gb), GB_IO_NR32, 0); // set volume to 0
						} 
					}
				}
				break;
			case 0x90:{ // MIDI_MSG_NOTE_ON
				// write reg
				//printf("Midi channel %u: note %u on.\n", channel, msg[1] & 0x7F);
				// "This time field [ev->time] is a timestamp, but not in real-world time units (like seconds or milliseconds). Instead, it's measured in frames relative to the start of the current audio block."
				
				noteOn[channel]=true;
				
				// check if the envelope values were changed by a note off. If it was, use self->userVol etc to set it back to the user's selected env values.
				if (channel != 2) {
					regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
					uint8_t tempVol = (regVal & 0xF0) >> 4;
					uint8_t tempEnvDirec = (regVal & 8) >> 3;
					uint8_t tempEnvLen = (regVal & 7);
					//printf("tempVol: %u\n", tempVol);
					if (/*tempVol == 0 &&*/ tempVol != self->userVol[channel] || tempEnvDirec != self->userEnvDirec[channel] || tempEnvLen != self->userEnvLen[channel]){ // I think this will be set because it's being written on the same frame as the note being triggered. (if the note is being triggered)
						regVal = 0; // make absolutely sure there's no stray bits.
						regVal |= (self->userVol[channel]) << 4;
						regVal |= (self->userEnvDirec[channel] << 3);
						regVal |= (self->userEnvLen[channel] & 7);
						GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
						//printf("set vol to %u\n", self->userVol[channel]);
					}
				} else {
					uint8_t tempVol = (GB_apu_read(&(self->gb), GB_IO_NR32) & 0b01100000) >> 5;
					if (tempVol != self->userVol[channel]){
						regVal = self->userVol[channel] << 5;
						GB_apu_write(&(self->gb), GB_IO_NR32, regVal);
					}
				}
				
				// play note
				uint8_t velocity = msg[2];
				if (velocity >= 64){isTrigger=true; noteTriggered[channel] = channel == 2 ? false : true;}
				newPitch = midiNoteAndPitchBend2gbPitch(msg[1] & 0x7F, self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST);
				writeNewPitchToAPU(&(self->gb), newPitch, channel, channel==2 ? false : isTrigger, 0xFF);
				
				self->lastMidiNote[channel] = msg[1] & 0x7F;
				break;
			}
			case 0xE0: // MIDI_MSG_PITCH
				if (channel!=3) {
					uint16_t midiPitchBend = (((uint16_t)msg[2] & 0x7F)<<7) | (msg[1] & 0x7F);
					//printf("Midi channel %u: pitch %04X\n", channel, midiPitchBend);
					newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], midiPitchBend, channel, self->NOISE_PITCH_LIST);
					// if the channel was already triggered at this pos, it should remain triggered. Otherwise, midi pitch bends will never retrigger the gb channel.
					if (noteTriggered[channel]) isTrigger=true;
					writeNewPitchToAPU(&(self->gb), newPitch, channel, isTrigger, 0xFF);
					self->lastMidiPitchBend[channel] = midiPitchBend;
				}
				break;
			default:
				break;
		}
	}
	
	// run the emulator for one audio frame, then send the output to the DAW
	GB_advance_cycles(&(self->gb), (uint8_t)(self->gb.apu_output.cycles_per_sample / 2)); // gb.apu_output.cycles_per_sample is doubled from what I expected it to be. Probably something to do with the word "sample" sometimes refering to an audio frame with left and right, and sometimes refering to a single sample from either the left OR right channel.
	// asssuming that Audio samples are normalized between -1.0 and 1.0
	float outputL = (float)((self->gb.apu_output.final_sample.left)) / (float)32768;
	float outputR = (float)((self->gb.apu_output.final_sample.right)) / (float)32768;
	return std::make_pair(outputL, outputR);
}
