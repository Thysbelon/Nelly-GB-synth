// The best way to just play a note is volume 0xF, env direc 0 (down), and env length 0. Setting env direc to 1 (up) with an env length of 0 messes with the volume and makes it switch between loud and quiet despite all the volume registers being the same.
// the best way to do a note off is volume 0, env direc 1, and env length 0. Volume 0 silences the channel. Have to set env direc to 1 for silencing a note because, even though env direc 1 messes up audible notes, you want env direc to be 1 when silencing a channel because if volume and env direc are BOTH 0, the channel's DAC will be turned off, and turning it back on causes a pop.
// TODO:
// Fix the little clicks in アルルの冒険 subsong 1 square 1.
// replace printf with the proper lv2 logger?

#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h> // LV2_ATOM_SEQUENCE_FOREACH. Need to read midi events
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h> // need this to map URIDs to integers, which I need in order to determine if an event is a midi event
#include <lv2/time/time.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h> // printf. NOTE to self: to view output, start reaper from the terminal.
#include <math.h> // round
#include "gb.h"
#include "gb_struct_def.h"
#include "apu.h"
#include "timing.h"

#define GAMEBOY_URI "https://github.com/Thysbelon/Nelly-GB-synth"

typedef struct { // only including these because they may improve performance
	LV2_URID atom_Path;
	LV2_URID atom_Sequence;
	LV2_URID atom_URID;
	LV2_URID atom_eventTransfer;
	//LV2_URID midi_Event;
} GameBoyPluginURIs;

#define GB_CLOCK_RATE 0x400000 // cycles per second
#define MAX_WAVES 127

typedef struct {
	GB_gameboy_t gb;
	double sampleRate;
	uint8_t NOISE_PITCH_LIST[127];
	uint8_t songWaveArray[MAX_WAVES][16]; // all wave data to be used by the song should be stored in a sysex message at the beginning. During the `run` method, if a sysex message is found, the plugin will take the sysex message, parse it as an array of wavetables, and store the result in this songWaveArray variable. Every time a CC21 message is detected, the plugin will use songWaveArray to write the correct wave to the APU. NOTE: the max number of waves is bottlenecked by CC21 which sets the index of the current wave to use; a CC can only go from 0-127, so there can be no more than 127 waves (and, even with garbage wave data, it is unlikely that a single song would have that many waves). TODO: if a song uses a musical sample (e.g. Pokemon Yellow samples pikachu voice clips. Music might sample drum sounds), is a max of 127 waves still enough? if not, I can always use two CC to create a 14-bit wave index selector.
	// to save space in memory, waves will be stored in the same format as gb: 32 samples long, with two 4-bit samples stored in each byte. However, the sysex message should store each 4-bit sample in its own byte, or else a wave containing the samples 0x0F and 0x07 right next to each other will be confused for the sysex end byte 0xF7.
	uint8_t curWaveIndex; // initialize this to 0
	bool legatoState[4 /* gb channels */]; // stores legatoState for each channel. When legatoState is true, new midi notes will only change pitch without retriggering the note.
	bool disableNoteOff[4];
	uint8_t userVol[4]; // DAW users expect that if they set a CC07 vol, it will remain until the next CC07 vol. This plugin has to overwrite the vol setting in the APU to handle noteOffs, so the user's set vol will be saved here. Every time a noteOn happens, if the vol in the APU is different than the user's set vol, re-write the user's set vol to the APU. userVol store volume in GB format, not midi format.
	uint8_t userEnvLen[4];
	uint8_t userEnvDirec[4];
	uint8_t userSoundLen[4];
	uint8_t lastMidiNote[4];
	uint16_t lastMidiPitchBend[4]; // 14-bit value
	float prevSpeed;
  float* outputLeft;
	float* outputRight;
	const LV2_Atom_Sequence* inMidi;
	const LV2_Atom_Sequence* inTime;
	
	LV2_URID_Map* map;
	LV2_URID midi_Event;
	LV2_URID time_speed; // used to detect if playback has paused.
	LV2_URID time_Position;
	LV2_URID atom_Object;
	LV2_URID atom_Float;
	
	GameBoyPluginURIs uris;
} GameBoyPlugin;

// private functions
static void resetInternalState(GameBoyPlugin* self, bool isInstantiate, double rate){
	memset(&(self->gb),0,sizeof(GB_gameboy_t));
	self->gb.model = GB_MODEL_DMG_B; // TODO: make it possible for the user to set the model.
	GB_apu_init(&(self->gb));
	if (isInstantiate && rate) {
		printf("DAW sample rate: %lf\n", rate);
		self->sampleRate=rate;
		GB_set_sample_rate(&(self->gb),(unsigned)(int)round(rate));
	} else if (self->sampleRate) {
		GB_set_sample_rate(&(self->gb),(unsigned)(int)round(self->sampleRate));
	} else {
		printf("Warning: GB sample rate not set!\n");
	}
	GB_set_highpass_filter_mode(&(self->gb), GB_HIGHPASS_ACCURATE); // the default mode is GB_HIGHPASS_OFF
	GB_apu_write(&(self->gb), GB_IO_NR10, 0);
	GB_apu_write(&(self->gb), GB_IO_NR52, 0x8f); // writing to bits 3-0 of this register *shouldn't* do anything because those bits are read only.
	GB_apu_write(&(self->gb), GB_IO_NR51, 0xFF);
	GB_apu_write(&(self->gb), GB_IO_NR50, 0x77);
	//set env
	GB_apu_write(&(self->gb), GB_IO_NR12, 0xF0);
	GB_apu_write(&(self->gb), GB_IO_NR22, 0xF0);
	//GB_apu_write(&(self->gb), GB_IO_NR32, 0);
	GB_apu_write(&(self->gb), GB_IO_NR42, 0xF0);
	// trigger channel
	GB_apu_write(&(self->gb), GB_IO_NR14, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR24, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR34, 0x80);
	GB_apu_write(&(self->gb), GB_IO_NR44, 0x80);
		
	GB_advance_cycles(&(self->gb), 0xFF); // TODO: reduce this cycle number to improve performance slightly.
		
	// trigger channel again with 0 vol. This shouldn't mess up the user playing notes, because this is the same state as after a note off.
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
		//12240
		if (self->gb.apu_output.final_sample.left == 0) break;
		if (i==0xFFFF-1) printf("loop never broke\n");
	}
	// advance past APU pop
	
	// I and users should avoid anything that turns the channel off. It will cause the next note played to be too loud
	
	self->prevSpeed=0;
	for (int i=0; i<4; i++){
		self->legatoState[i]=false;
		self->disableNoteOff[i]=false;
		self->userVol[i]=0x0F; // GB format!
		self->userEnvLen[i]=0; 
		self->userEnvDirec[i]=0; // an env direc of "up" and an env length of 0 leads to volume weirdness.
		self->userSoundLen[i]=0;
		self->lastMidiNote[i]=72; // C4
		self->lastMidiPitchBend[i]=0x2000; // center.
	}
	self->userVol[2]=1;
	self->curWaveIndex = 0;
	if (isInstantiate==true) {
		for (uint8_t i=0; i<MAX_WAVES; i++){ // if the wave sysex message is only sent at the start of the song, then the wave channel might break if the user pauses and resumes in the middle of a song, which would be bad. Maybe I can get away with not resetting wave stuff during activate()? NOTE: I think I can make wave stuff persist if I turn it into a user-facing control rather than an internal state.
			for (uint8_t i2=0; i2<16; i2++){
				self->songWaveArray[i][i2]=0;
			}
		}
	}
}

uint16_t midiNoteAndPitchBend2gbPitch(uint8_t midiNote, uint16_t midiPitchBend, uint8_t channel, uint8_t NOISE_PITCH_LIST[]){
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

void writeNewPitchToAPU(GB_gameboy_t* gbPointer, uint16_t newPitch, uint8_t channel, bool isTrigger, uint8_t soundLenEn){
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

uint8_t convertMidiValToRange(uint8_t inMidiVal, uint8_t outValMax){ // used to convert midi vals to register bit val range.
	const uint8_t MIDI_CC_MAX = 0x7F;
	return (uint8_t)round((float)outValMax * ((float)inMidiVal / MIDI_CC_MAX));
}
// private functions end

static LV2_Handle instantiate(const LV2_Descriptor*     descriptor,
            double                    rate, // DAW sample rate
            const char*               bundle_path,
            const LV2_Feature* const* features) {
	GameBoyPlugin* self = calloc(1, sizeof(GameBoyPlugin));
	resetInternalState(self, true, rate);
	
	uint8_t noisePitchListSize=0;
	//uint8_t NOISE_PITCH_LIST[127];
	//for (uint8_t i=0; i<0xF7; i++){
	for (int16_t noisePitch=0xF7; noisePitch >= 0; noisePitch--){ // the list should be written backwards because lower values tend to be higher pitched. 0xF7 is 0b11110111.
		if ((noisePitch & 8) == 0) {
			self->NOISE_PITCH_LIST[noisePitchListSize]=(uint8_t)noisePitch;
			noisePitchListSize++;
		}
	}
	
	// map midi event URI to integer, so that later I can compare ev->body.type to the midi event URI integer.
	
	// Scan host features for URID map
  // clang-format off
  const char*  missing = lv2_features_query(
    features,
    //LV2_LOG__log,  &self->logger.log, false,
    LV2_URID__map, &self->map,        true,
    NULL);
  // clang-format on

  //lv2_log_logger_set_map(&self->logger, self->map);
  if (missing) {
    //lv2_log_error(&self->logger, "Missing feature <%s>\n", missing);
		fprintf(stderr, "Missing feature <%s>\n", missing);
    free(self);
    return NULL;
  }
	
	self->midi_Event = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
	self->time_speed = self->map->map(self->map->handle, LV2_TIME__speed);
	self->time_Position = self->map->map(self->map->handle, LV2_TIME__Position);
	self->atom_Object = self->map->map(self->map->handle, LV2_ATOM__Object);
	self->atom_Float = self->map->map(self->map->handle, LV2_ATOM__Float);
	
	self->uris.atom_Path = self->map->map(self->map->handle, LV2_ATOM__Path);
	self->uris.atom_Sequence = self->map->map(self->map->handle, LV2_ATOM__Sequence);
	self->uris.atom_URID = self->map->map(self->map->handle, LV2_ATOM__URID);
	self->uris.atom_eventTransfer = self->map->map(self->map->handle, LV2_ATOM__eventTransfer);
	
	return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
	GameBoyPlugin* self = (GameBoyPlugin*)instance;
	switch (port) {
		case 0:
			self->inMidi = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->inTime = (const LV2_Atom_Sequence*)data;
			//printf("Connected port %d to address %p\n", port, data);
			break;
		case 2:
			self->outputLeft = (float*)data;
			//printf("Connected port %d to address %p\n", port, data);
			break;
		case 3:
			self->outputRight = (float*)data;
			//printf("Connected port %d to address %p\n", port, data);
			break;
		default:
			break;
	}
}

static void
activate(LV2_Handle instance) {
	printf("activate called.\n");
	resetInternalState((GameBoyPlugin*)instance, false, 0);
}

static void run(LV2_Handle instance, uint32_t n_samples) { // most of the code should be in here. n_samples refers to audio frames, not interleaved samples.
	GameBoyPlugin* self = (GameBoyPlugin*)instance;
#define MIDI_MSG_PITCH LV2_MIDI_MSG_BENDER // just want to use a different name.
#define MAX_EVS 0xFF
#define EV_I_TYPE uint8_t
	LV2_Atom_Event* midiEvArray[MAX_EVS];
	EV_I_TYPE midiEvArrSize=0;
	
	// events are looped through in the order that they happen chronologically
	// midi events are scheduled for different times in the current run call.
	// store incoming scheduled midi events for later.
	LV2_ATOM_SEQUENCE_FOREACH (self->inMidi, ev) {
		if (ev->body.type == self->midi_Event /*midi event URI mapped to an integer*/) {
			midiEvArray[midiEvArrSize]=ev;
			if (midiEvArrSize < MAX_EVS) midiEvArrSize++;
		}
	}

  // Render audio
	for (uint32_t pos = 0; pos < n_samples; pos++) { // when there is no audio to be rendered, output silence. If I don't do this, the plugin constantly outputs garbage noise.
		self->outputLeft[pos] = 0;
		self->outputRight[pos] = 0;
		// compare pos (measure of elapsed time / progress through this current audio block) with the scheduledTime of each event in this run cycle
		LV2_Atom_Event* curPosMidiEvs[MAX_EVS]; // use this to reorder conflicting simultaneous midi events
		EV_I_TYPE curPosMidiEvsSize=0;
		for (EV_I_TYPE evI=0; evI<midiEvArrSize; evI++) {
			if (midiEvArray[evI]==NULL) continue;
			if (pos >= midiEvArray[evI]->time.frames) {
				curPosMidiEvs[curPosMidiEvsSize] = midiEvArray[evI];
				if (curPosMidiEvsSize < MAX_EVS) curPosMidiEvsSize++;
				midiEvArray[evI]=NULL;
			} /* else if (midiEvArray[evI]->time.frames > pos) {
				break; // events in midiEvArray are stored in chronological order, so once we reach the first event that is scheduled after the current position, we can assume that the rest of the events in the array are also not ready to be played.
			}
			*/
		}
		// now that we have collected all the midi events that happen simultaneously on this position, we can convert them into APU writes.
		// these boolean arrays exist to make sure that simultaneous events don't accidently overwrite each other.
		bool volSet[4]={false, false, false, false};
		bool noteOn[4]={false, false, false, false}; // a note on was sent at this position
		bool noteTriggered[4]={false, false, false, false};
		bool pitchBended[4]={false, false, false, false};
		for (EV_I_TYPE evI=0; evI<curPosMidiEvsSize; evI++) {
			const uint8_t* const msg = (const uint8_t*)(curPosMidiEvs[evI] + 1); // ev is a pointer to the event. Once the event has been identified as a midi event, advance the pointer one byte forward and save the result as a new pointer to the midi message. https://stackoverflow.com/a/12100686/20697953
			// The midi message has a variable length. The first byte is always the status byte.
			uint8_t channel=0xFF; // 0xFF is a marker for messages with no channel.
			if (lv2_midi_is_voice_message(msg)){
				channel = (*msg) & 0x0F; // https://michd.me/jottings/midi-message-format-reference/
				if (channel > 3) channel=0;
			}
			// TODO: check if APU exists before running APU methods
			// convert midi event to gb apu register write
			uint16_t newPitch=0;
			bool isTrigger=false;
			uint8_t regVal=0;
			switch (lv2_midi_message_type(msg)) { /*when used without any brackets or dereferencers, msg acts as a pointer to the first byte of the midi message A.K.A. the status byte.*/
				case LV2_MIDI_MSG_SYSTEM_EXCLUSIVE: // A.K.A. sysex. TODO: figure out how various midi device manufacturers uniquely identify their sysex messages so that they don't get mistaken for anything else.
					for (uint8_t i=0; i<MAX_WAVES; i++){ // If I'm not resetting wave data during activate(), I need to reset it when a sysex message is received.
						for (uint8_t i2=0; i2<16; i2++){
							self->songWaveArray[i][i2]=0;
						}
					}
					
					printf("sysex message received. Collecting waves...\n");
					bool breakImmediately=false; // I could use a goto instead, but this feels safer.
					for (uint8_t waveI=0; waveI<MAX_WAVES; waveI++) {
						printf("Wave %u:\n", waveI);
						for (uint8_t samplePairI=0; samplePairI<16; samplePairI++) {
							if (msg[1+waveI*32+samplePairI*2]==0xF7) {breakImmediately=true; break;} // end of sysex
							if (msg[1+waveI*32+samplePairI*2+1]==0xF7) {breakImmediately=true; break;} // end of sysex
							self->songWaveArray[waveI][samplePairI] = (msg[1+waveI*32+samplePairI*2] << 4) | msg[1+waveI*32+samplePairI*2+1];
							printf("%02X ", self->songWaveArray[waveI][samplePairI]);
						}
						if (breakImmediately==true) break;
						printf("\n");
					}
					printf("\nend of sysex\n");
					break;
				case LV2_MIDI_MSG_CONTROLLER:
					// msg[1] control number
					// msg[2] control value
					switch (msg[1]){
						case LV2_MIDI_CTL_MSB_MAIN_VOLUME:
							volSet[channel]=true;
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
							printf("midi vol: %u, new userVol: %u\n", msg[2], self->userVol[channel]);
							GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
							break;
						case 9: /* gb pan mute (0 0) */
							if (msg[2] >= 64) {
								regVal=GB_apu_read(&(self->gb), GB_IO_NR51);
								regVal &= (uint8_t)((~(0x11 << channel)) & 0xFF); // discard only the bits currently being modified.
								GB_apu_write(&(self->gb), GB_IO_NR51, regVal);
							}
							break;
						case LV2_MIDI_CTL_MSB_PAN:
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
						case LV2_MIDI_CTL_MSB_EFFECT1: /*cc12 envelope direction*/
							if (channel!=2) {
								regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
								regVal &= 0b11110111; // keep env start vol and envLen
								//self->userEnvDirec[channel] = msg[2] ? 1 : 0;
								if (msg[2] >= 64) {
									self->userEnvDirec[channel]=1;
								} else {
									self->userEnvDirec[channel]=0;
								}
								regVal |= (self->userEnvDirec[channel] << 3);
								GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
							}
							break;
						case LV2_MIDI_CTL_MSB_EFFECT2: /*cc13 envelope length*/
							if (channel!=2) {
								regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
								regVal &= 0b11111000; // keep env start vol and envDir
								self->userEnvLen[channel] = (uint8_t)round(((float)msg[2] / 0x7F) * 7) & 0b00000111;
								regVal |= self->userEnvLen[channel];
								GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
							}
							break;
						// TODO: find a gbs to TEST sound length and sweep settings.
						case 14: /*sound length enable*/
							uint8_t soundLenEn = msg[2] >= 64 ? 1 : 0;
							newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
							writeNewPitchToAPU(&(self->gb), newPitch, channel, noteTriggered[channel], soundLenEn);
							break;
						case 15: /*sound length*/ // TODO: verify that the correct value is being written to the GB APU register; it sounds a bit short.
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
							break;
						case LV2_MIDI_CTL_MSB_GENERAL_PURPOSE1: // sweep speed
							if (channel == 0){
								uint8_t sweepSpeed = convertMidiValToRange(msg[2], 7);
								regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
								regVal &= 0b00001111;
								regVal |= ((sweepSpeed & 0b111) << 4);
								GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
							}
							break;
						case LV2_MIDI_CTL_MSB_GENERAL_PURPOSE2: // sweep shift
							if (channel == 0){
								uint8_t sweepShift = convertMidiValToRange(msg[2], 7);
								regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
								regVal &= 0b01111000;
								regVal |= (sweepShift & 0b111);
								GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
							}
							break;
						case LV2_MIDI_CTL_MSB_GENERAL_PURPOSE3: // sweep up or down. TODO: for readability, pitch should go up when cc18 is 127, and down when cc18 is 0
							if (channel == 0){
								uint8_t sweepDir = convertMidiValToRange(msg[2], 1);
								regVal = GB_apu_read(&(self->gb), GB_IO_NR10);
								regVal &= 0b01110111;
								regVal |= ((sweepDir & 1) << 3);
								GB_apu_write(&(self->gb), GB_IO_NR10, regVal);
							}
							break;
						case LV2_MIDI_CTL_MSB_GENERAL_PURPOSE4: // duty cycle A.K.A. pulse width
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
							self->curWaveIndex=msg[2];
							// TODO: do I need curWaveIndex? Maybe I should just write the new wave to the APU when I receive CC21.
							
							// turn off DAC
							// write to wave ram
							// turn on DAC
							// trigger channel
							// TODO: wave should ONLY be triggered when switching waves. Triggering it at any other time will unpredictably corrupt wave ram.
							// TODO: does wave need to be re-triggered to change the volume? My midi output suggests that it doesn't need to be re-triggered, but pandocs implies that it does: "Trigger (Write-only): Writing any value to NR34 with this bit set triggers the channel, causing the following to occur:.. ...Volume is set to contents of NR32 initial volume."
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
							break;
						case 22: // disable note off
							if (msg[2] >= 64) {
								self->disableNoteOff[channel]=true;
							} else {
								self->disableNoteOff[channel]=false;
							}
							break;
						case LV2_MIDI_CTL_LEGATO_FOOTSWITCH: // when on, new notes will only change pitch without retriggering the note.
							if (channel<4) {
								if (msg[2] >= 64) {
									self->legatoState[channel] = true;
									if (noteOn[channel]==true) { // when a legatoState change happens at the same time as a note, the note should immediately be affected by the legatoState change.
										newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
										writeNewPitchToAPU(&(self->gb), newPitch, channel, false, 0xFF);
									}
								} else {
									self->legatoState[channel] = false;
									if (noteOn[channel]==true) { // when a legatoState change happens at the same time as a note, the note should immediately be affected by the legatoState change.
										newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
										writeNewPitchToAPU(&(self->gb), newPitch, channel, true, 0xFF);
										noteTriggered[channel]=true;
									}
								}
							}
							break;
						default:
							break;
					}
					break;
				case LV2_MIDI_MSG_NOTE_OFF:
					if (noteOn[channel]==false && self->legatoState[channel]==false && self->disableNoteOff[channel]==false) { // ?
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
								//case 2:
								//	curVol=GB_apu_read(&(self->gb), GB_IO_NR32) & 0b01100000; // exact number doesn't matter, I'm just checking if this is zero or not
								//	break;
								case 3:
									curVol=self->gb.apu.noise_channel.current_volume;
									break;
								default:
									break;
							}
							if (GB_apu_read(&(self->gb), GB_IO_NR52) & (0b00000001 << channel) && !((curVol==0 && envDirec == 0/*down*/) || (curVol==0 && envLen==0))) { // if channel is enabled AND the current volume is greater than 0. make sure a false positive doesn't happen when a channel starts at 0 vol then goes up via envelope.
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
								//newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST); // pitch is write-only. rewrite pitch so it isn't lost.
								//writeNewPitchToAPU(&(self->gb), newPitch, channel, true); // have to retrigger the channel for the silence to take effect.
								//noteTriggered[channel]=true;
							} 
						}
					}
					break;
				case LV2_MIDI_MSG_NOTE_ON: 
					// write reg
					printf("Midi channel %u: note %u on. Scheduled Time: %ld. pos: %u. n_samples: %u\n", channel, msg[1] & 0x7F, curPosMidiEvs[evI]->time.frames, pos, n_samples);
					// "This time field [ev->time] is a timestamp, but not in real-world time units (like seconds or milliseconds). Instead, it's measured in frames relative to the start of the current audio block."
					
					noteOn[channel]=true;
					
					// check if the envelope values were changed by a note off. If it was, use self->userVol etc to set it back to the user's selected env values.
					if (channel != 2) {
						regVal = GB_apu_read(&(self->gb), GB_IO_NR12 + channel*5);
						uint8_t tempVol = (regVal & 0xF0) >> 4;
						uint8_t tempEnvDirec = (regVal & 8) >> 3;
						uint8_t tempEnvLen = (regVal & 7);
						printf("tempVol: %u\n", tempVol);
						if (/*tempVol == 0 &&*/ tempVol != self->userVol[channel] || tempEnvDirec != self->userEnvDirec[channel] || tempEnvLen != self->userEnvLen[channel]){ // I think this will be set because it's being written on the same frame as the note being triggered. (if the note is being triggered)
							regVal = 0; // make absolutely sure there's no stray bits.
							regVal |= (self->userVol[channel]) << 4;
							regVal |= (self->userEnvDirec[channel] << 3);
							regVal |= (self->userEnvLen[channel] & 7);
							GB_apu_write(&(self->gb), GB_IO_NR12 + channel*5, regVal);
							printf("set vol to %u\n", self->userVol[channel]);
						}
					} else {
						uint8_t tempVol = (GB_apu_read(&(self->gb), GB_IO_NR32) & 0b01100000) >> 5;
						if (tempVol != self->userVol[channel]){
							regVal = self->userVol[channel] << 5;
							GB_apu_write(&(self->gb), GB_IO_NR32, regVal);
						}
					}
					
					// play note
					if (self->legatoState[channel] == false){isTrigger=true; noteTriggered[channel] = channel == 2 ? false : true;}
					newPitch = midiNoteAndPitchBend2gbPitch(msg[1] & 0x7F, self->lastMidiPitchBend[channel], channel, self->NOISE_PITCH_LIST);
					writeNewPitchToAPU(&(self->gb), newPitch, channel, channel==2 ? false : isTrigger, 0xFF);
					
					self->lastMidiNote[channel] = msg[1] & 0x7F;
					break;
				case MIDI_MSG_PITCH:
					if (channel!=3) {
						uint16_t midiPitchBend = (((uint16_t)msg[2] & 0x7F)<<7) | (msg[1] & 0x7F);
						printf("Midi channel %u: pitch %04X\n", channel, midiPitchBend);
						newPitch = midiNoteAndPitchBend2gbPitch(self->lastMidiNote[channel], midiPitchBend, channel, self->NOISE_PITCH_LIST);
						// if the channel was already triggered at this pos, it should remain triggered. Otherwise, midi pitch bends will never retrigger the gb channel.
						if (noteTriggered[channel]) isTrigger=true;
						writeNewPitchToAPU(&(self->gb), newPitch, channel, isTrigger, 0xFF);
						pitchBended[channel]=true;
						self->lastMidiPitchBend[channel] = midiPitchBend;
					}
					break;
				default:
					break;
			}
			// TODO: store the values of these midi events and almost all internal state stuff inside user-facing controls so that they persist after pausing and resuming.
			//curPosMidiEvs[evI]=NULL; // remove processed event from list
		}
		GB_advance_cycles(&(self->gb), (uint8_t)(self->gb.apu_output.cycles_per_sample / 2)); // gb.apu_output.cycles_per_sample is doubled from what I expected it to be. Probably something to do with the word "sample" sometimes refering to an audio frame with left and right, and sometimes refering to a single sample from either the left OR right channel.
		//The sample is rendered to gb.apu_output.final_sample as an int16
		// LV2: "Audio samples are normalized between -1.0 and 1.0"
		self->outputLeft[pos] = (float)((self->gb.apu_output.final_sample.left)) / (float)32768;
		self->outputRight[pos] = (float)((self->gb.apu_output.final_sample.right)) / (float)32768;
	}
	
	LV2_ATOM_SEQUENCE_FOREACH (self->inTime, ev) {
		// Check if this event is an Object
		if (ev->body.type == self->atom_Object) {
			const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == self->time_Position) {
				// Received position information, update
				//update_position(self, obj);
				LV2_Atom* speed = NULL;
				lv2_atom_object_get(obj,
					self->time_speed, &speed,
					NULL);
				if (speed && speed->type == self->atom_Float) {
					// Speed changed, e.g. 0 (stop) to 1 (play)
					float curSpeed;
					curSpeed = ((LV2_Atom_Float*)speed)->body;
					if (curSpeed != self->prevSpeed) {
						if (curSpeed == 0) {
							resetInternalState(self, false, 0);
						} else {
							self->prevSpeed = curSpeed;
						}
					}
				}
			}
		}
	}
}

static void deactivate(LV2_Handle instance) {
	printf("deactivate called.\n");
}

static void cleanup(LV2_Handle instance) {
    GameBoyPlugin* self = (GameBoyPlugin*)instance;
    //apu_cleanup(&self->apu);
		//free(&(self->gb)); // "double free or corruption (!prev)"
    free(self);
}

static const LV2_Descriptor descriptor = {GAMEBOY_URI,
                                          instantiate,
                                          connect_port,
                                          activate,
                                          run,
                                          deactivate,
                                          cleanup,
                                          NULL}; // extension_data 

LV2_SYMBOL_EXPORT const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
