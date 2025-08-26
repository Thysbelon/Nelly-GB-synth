// The best way to just play a note is volume 0xF, env direc 0 (down), and env length 0. Setting env direc to 1 (up) with an env length of 0 messes with the volume and makes it switch between loud and quiet despite all the volume registers being the same.
// the best way to do a note off is volume 0, env direc 1, and env length 0. Volume 0 silences the channel. Have to set env direc to 1 for silencing a note because, even though env direc 1 messes up audible notes, you want env direc to be 1 when silencing a channel because if volume and env direc are BOTH 0, the channel's DAC will be turned off, and turning it back on causes a pop.
// TODO:
// make it so midi events with a channel above 3 (0-indexed) are ignored rather than treated as being on channel 0
// try to simply redundant code across the lv2 and clap version of the plugin? (The clap version code has a few more touch-ups than the lv2 version)
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
#include "plugin-core.hpp"

#define GAMEBOY_URI "https://github.com/Thysbelon/Nelly-GB-synth"

typedef struct { // only including these because they may improve performance
	LV2_URID atom_Path;
	LV2_URID atom_Sequence;
	LV2_URID atom_URID;
	LV2_URID atom_eventTransfer;
	//LV2_URID midi_Event;
} GameBoyPluginURIs;

typedef struct {
	GameBoyPluginCore core; // The part of the plugin that is standard agnostic
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

static LV2_Handle instantiate(const LV2_Descriptor*     descriptor,
            double                    rate, // DAW sample rate
            const char*               bundle_path,
            const LV2_Feature* const* features) {
	GameBoyPlugin* self = (GameBoyPlugin*)calloc(1, sizeof(GameBoyPlugin));
	resetInternalState(&(self->core), rate, true);
	self->prevSpeed = 0;
	
	setUpNoisePitchList(&(self->core));
	
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
	resetInternalState(&(((GameBoyPlugin*)instance)->core), 0, false);
	((GameBoyPlugin*)instance)->prevSpeed = 0;
}

static void run(LV2_Handle instance, uint32_t n_samples) { // most of the code should be in here. n_samples refers to audio frames, not interleaved samples.
	GameBoyPlugin* self = (GameBoyPlugin*)instance;
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
		
		std::vector<midiMessage> curFrameMidiEvsGeneric;
		for (EV_I_TYPE evI=0; evI<curPosMidiEvsSize; evI++) {
			midiMessage newEv;
			newEv.statusByte = 0; // marker for an invalid event
			const uint8_t* const msg = (const uint8_t*)(curPosMidiEvs[evI] + 1); // ev is a pointer to the event. Once the event has been identified as a midi event, advance the pointer one byte forward and save the result as a new pointer to the midi message.
			newEv.statusByte = msg[0];
			if (newEv.statusByte == 0xF0) { // sysex
				for (int i = 1; i<0xFFFFFFFF; i++){ // effectively a while loop with a failsafe
					if (msg[i] == 0xF7) break;
					newEv.dataBytes.push_back(msg[i]);
				}
			} else {
				for (int i=0; i<2; i++){
					newEv.dataBytes.push_back(msg[i+1]);
				}
			}
			curFrameMidiEvsGeneric.push_back(newEv);
		}
		
		// now that we have collected all the midi events that happen simultaneously on this position, we can convert them into APU writes.
		std::pair<float, float> outputs = processFrame(&(self->core), curFrameMidiEvsGeneric);
		
		// LV2: "Audio samples are normalized between -1.0 and 1.0"
		self->outputLeft[pos] = outputs.first;
		self->outputRight[pos] = outputs.second;
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
							resetInternalState(&(self->core), 0, false);
							self->prevSpeed = 0;
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
