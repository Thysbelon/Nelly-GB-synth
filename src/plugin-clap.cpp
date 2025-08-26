// https://nakst.gitlab.io/tutorial/clap-part-1.html
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <vector>
#include "clap/clap.h"
#include "gb.h"
#include "gb_struct_def.h"
#include "apu.h"
#include "timing.h"
#include "plugin-core.hpp"

struct GameBoyPlugin {
	clap_plugin_t plugin;
	const clap_host_t *host;
	
	GameBoyPluginCore core; // The part of the plugin that is standard agnostic
	bool prevPlaying;
};

static const clap_plugin_descriptor_t pluginDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "Thysbelon.NellyGB",
	.name = "Nelly GB",
	.vendor = "Thysbelon",
	.url = "https://github.com/Thysbelon/Nelly-GB-synth",
	.manual_url = "https://github.com/Thysbelon/Nelly-GB-synth",
	.support_url = "https://github.com/Thysbelon/Nelly-GB-synth",
	.version = "1.1.0",
	.description = "A synthesizer plugin that emulates the Game Boy's APU and converts Midi events into Game Boy APU register writes.",

	.features = (const char *[]) {
		CLAP_PLUGIN_FEATURE_INSTRUMENT,
		CLAP_PLUGIN_FEATURE_SYNTHESIZER,
		CLAP_PLUGIN_FEATURE_STEREO,
		NULL,
	},
};

static const clap_plugin_note_ports_t extensionNotePorts = {
	.count = [] (const clap_plugin_t *plugin, bool isInput) -> uint32_t {
		return isInput ? 1 : 0;
	},

	.get = [] (const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_note_port_info_t *info) -> bool {
		if (!isInput || index) return false;
		info->id = 0;
		info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
		info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
		snprintf(info->name, sizeof(info->name), "%s", "Note Port");
		return true;
	},
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
	.count = [] (const clap_plugin_t *plugin, bool isInput) -> uint32_t { 
		return isInput ? 0 : 1; 
	},

	.get = [] (const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_audio_port_info_t *info) -> bool {
		if (isInput || index) return false;
		info->id = 0;
		info->channel_count = 2;
		info->flags = CLAP_AUDIO_PORT_IS_MAIN;
		info->port_type = CLAP_PORT_STEREO;
		info->in_place_pair = CLAP_INVALID_ID;
		snprintf(info->name, sizeof(info->name), "%s", "Audio Output");
		return true;
	},
};

static const clap_plugin_t pluginClass = { // contains all of the plugin methods that will be called by the DAW
	.desc = &pluginDescriptor,
	.plugin_data = nullptr,

	.init = [] (const clap_plugin *_plugin) -> bool {
		GameBoyPlugin *self = (GameBoyPlugin *) _plugin->plugin_data;
		
		setUpNoisePitchList(&(self->core));
		
		return true;
	},

	.destroy = [] (const clap_plugin *_plugin) {
		GameBoyPlugin *plugin = (GameBoyPlugin *) _plugin->plugin_data;
		free(plugin);
	},

	.activate = [] (const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount, uint32_t maximumFramesCount) -> bool { 
		GameBoyPlugin *self = (GameBoyPlugin *) _plugin->plugin_data;
		resetInternalState(&(self->core), sampleRate);
		self->prevPlaying=false;
		return true;
	},

	.deactivate = [] (const clap_plugin *_plugin) {
	},

	.start_processing = [] (const clap_plugin *_plugin) -> bool {
		return true;
	},

	.stop_processing = [] (const clap_plugin *_plugin) {
	},

	.reset = [] (const clap_plugin *_plugin) {
		GameBoyPlugin *self = (GameBoyPlugin *) _plugin->plugin_data;
		resetInternalState(&(self->core), 0);
		self->prevPlaying=false;
	},

	.process = [] (const clap_plugin *_plugin, const clap_process_t *process) -> clap_process_status { 
		GameBoyPlugin *self = (GameBoyPlugin *) _plugin->plugin_data;
		
		assert(process->audio_outputs_count == 1);
		assert(process->audio_inputs_count == 0);

		const uint32_t frameCount = process->frames_count;
		float *outputL;
		float *outputR;
		outputL = process->audio_outputs[0].data32[0];
		outputR = process->audio_outputs[0].data32[1];
		
		// put this block's midi events in a regular array to make processing easier.
		std::vector<const clap_event_header_t*> midiEvArray; // contains both CLAP_EVENT_MIDI and CLAP_EVENT_MIDI_SYSEX
		{
			const uint32_t inputEventCount = process->in_events->size(process->in_events); // transport events are NEVER contained here. Only one transport event is sent per block, in process->transport
			for (uint32_t eventIndex = 0; eventIndex<inputEventCount; eventIndex++){
				const clap_event_header_t *event = process->in_events->get(process->in_events, eventIndex);
				if (event->type == CLAP_EVENT_MIDI || event->type == CLAP_EVENT_MIDI_SYSEX){
					midiEvArray.push_back(event);
				}
			}
		}
		
		for (uint32_t curFrame = 0; curFrame<frameCount; curFrame++){
			// output silence
			outputL[curFrame] = 0;
			outputR[curFrame] = 0;
			
			// store all midi events that happen at the same frame in a sub-vector
			std::vector<const clap_event_header_t*> curFrameMidiEvs; // use this to reorder conflicting simultaneous midi events
			for (uint32_t evI=0; evI<midiEvArray.size(); evI++) {
				if (midiEvArray[evI]==NULL) continue;
				if (curFrame >= midiEvArray[evI]->time) {
					curFrameMidiEvs.push_back(midiEvArray[evI]);
					midiEvArray[evI]=NULL;
				} /* else if (midiEvArray[evI]->time.frames > pos) {
					break; // events in midiEvArray are stored in chronological order, so once we reach the first event that is scheduled after the current position, we can assume that the rest of the events in the array are also not ready to be played.
				}
				*/
			}
			
			std::vector<midiMessage> curFrameMidiEvsGeneric;
			for (uint32_t evI=0; evI<curFrameMidiEvs.size(); evI++) { // convert
				midiMessage newEv;
				newEv.statusByte = 0; // marker for an invalid event
				if (curFrameMidiEvs[evI]->type == CLAP_EVENT_MIDI_SYSEX) {
					newEv.statusByte = 0xF0;
					const uint8_t* sysexData = ((clap_event_midi_sysex_t*)curFrameMidiEvs[evI])->buffer;
					const uint32_t sysexSize = ((clap_event_midi_sysex_t*)curFrameMidiEvs[evI])->size;
					if (sysexData[0] == 0xF0) {
						sysexData++; // move the pointer forward one byte.
						printf("Had to advance sysexData pointer\n");
					}
					for (uint32_t i=0; i<sysexSize; i++){
						if (sysexData[i]==0xF7) break;
						newEv.dataBytes.push_back(sysexData[i]);
					}
				} else if (curFrameMidiEvs[evI]->type == CLAP_EVENT_MIDI) {
					newEv.statusByte = (((clap_event_midi_t*)curFrameMidiEvs[evI])->data)[0];
					for (int i=0; i<2; i++){
						newEv.dataBytes.push_back((((clap_event_midi_t*)curFrameMidiEvs[evI])->data)[i+1]);
					}
				}
				curFrameMidiEvsGeneric.push_back(newEv);
			}
			
			// now that we have collected all the midi events that happen simultaneously on this position, we can convert them into APU writes.
			std::pair<float, float> outputs = processFrame(&(self->core), curFrameMidiEvsGeneric);
			outputL[curFrame] = outputs.first;
			outputR[curFrame] = outputs.second;
		}
		
		// check if the DAW has just paused. If true, call resetInternalState
		const clap_event_transport_t* blockTransportEvent;
		blockTransportEvent = process->transport;
		if (blockTransportEvent != nullptr) {
			bool isPlaying = ((blockTransportEvent->flags) & CLAP_TRANSPORT_IS_PLAYING) ? true : false;
			if (isPlaying != self->prevPlaying) {
				if (isPlaying == false) {
					printf("isPlaying == false\n");
					resetInternalState(&(self->core), 0);
					self->prevPlaying=false;
				} else {
					printf("isPlaying == true\n");
					self->prevPlaying = isPlaying;
				}
			}
		}
		
		return CLAP_PROCESS_CONTINUE;
	},

	.get_extension = [] (const clap_plugin *plugin, const char *id) -> const void * {
		if (0 == strcmp(id, CLAP_EXT_NOTE_PORTS )) return &extensionNotePorts;
		if (0 == strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &extensionAudioPorts;
		return nullptr;
	},

	.on_main_thread = [] (const clap_plugin *_plugin) {
	},
};

static const clap_plugin_factory_t pluginFactory = {
	.get_plugin_count = [] (const clap_plugin_factory *factory) -> uint32_t { 
		return 1; 
	},

	.get_plugin_descriptor = [] (const clap_plugin_factory *factory, uint32_t index) -> const clap_plugin_descriptor_t * { 
		return index == 0 ? &pluginDescriptor : nullptr; 
	},

	.create_plugin = [] (const clap_plugin_factory *factory, const clap_host_t *host, const char *pluginID) -> const clap_plugin_t * {
		if (!clap_version_is_compatible(host->clap_version) || strcmp(pluginID, pluginDescriptor.id)) {
			return nullptr;
		}

		GameBoyPlugin *plugin = (GameBoyPlugin *) calloc(1, sizeof(GameBoyPlugin));
		plugin->host = host;
		plugin->plugin = pluginClass;
		plugin->plugin.plugin_data = plugin;
		return &plugin->plugin;
	},
};

extern "C" const clap_plugin_entry_t clap_entry = {
	.clap_version = CLAP_VERSION_INIT,

	.init = [] (const char *path) -> bool { 
		return true; 
	},

	.deinit = [] () {},

	.get_factory = [] (const char *factoryID) -> const void * {
		return strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &pluginFactory;
	},
};
