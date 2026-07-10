# Nelly GB Synth

**Notice**: this virtual instrument is basically finished and is unlikely to receive any new features, but any future bug fixes will likely only be made for [Thaumoc](https://github.com/Thysbelon/Thaumoc).

An LV2 and CLAP synthesizer plugin that emulates the Game Boy's APU and converts Midi events into Game Boy APU register writes.

Compatible midi files can be obtained by using [gbs2midi](https://github.com/Thysbelon/gbs2midi).

## Midi Event to Game Boy APU Register Reference

- SysEx:  
	Contains all of the wavetable data used in a song. This event should be placed at the start of a midi file.  
	The wave data consists of values from 0x00 to 0x0F.  
	example of a sysex message that contains two waves (line breaks added.):  
	```
	F0
	0F 0F 0F 0F 0F 0D 0B 08 05 03 01 00 00 00 00 00 00 00 00 00 00 01 03 05 08 0B 0D 0F 0F 0F 0F 0F 
	00 00 00 00 00 00 00 00 00 00 00 00 0F 0F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0F 0F 00 00 
	F7
	```  
	The plugin doesn't have a user interface, so I recommend using [Furnace](https://github.com/tildearrow/furnace)'s wavetable editor to create waves, then copy and paste the hexadecimal representation of the wave data into a midi sysex message.
- Midi Note On:  
	This midi event affects multiple GB registers:  
	- Sets the pitch to the pitch of the note
	- Triggers the channel if the velocity of the note is greater than or equal to 64
	- Resets the volume (and envelope) of the channel if it was silenced by a note off event. The values are reset to whatever the last CC-defined value was.
- Midi Note Off:  
	Silences the channel.  
	(On channels with an envelope, envelope direction will be set to "up" and envelope length will be set to zero.)  
	Because the Game Boy APU doesn't have anything like a note off, the plugin is designed so that midi note off events only have a temporary effect on the Game Boy APU. If the volume (or envelope) is changed or another note is played, the channel will no longer be silent.
- Pitch Bend:  
	Changes the pitch of a channel without triggering it.  
	Currently, this plugin does not allow the user to change the pitch bend range, but this will be added in a future release. In the meantime, the user can create large pitch bends by using Legato Mode.
- Volume MSB (CC07):  
	Sets the volume of the channel
- Pan (CC10):  
	Sets the stereo panning of the channel. The Game Boy can only pan hard left, hard right, or center.
- Pan Mute (CC09) (custom):  
	In the Game Boy APU, it is possible to mute a channel by setting the panning value to 0. The Midi pan control does not have this functionality, so CC09 sets the panning register to 0 when CC09 is 127.
- Envelope Direction (CC12) (custom):  
	Sets whether the volume envelope should gradually increase or decrease volume. 127 is increase, 0 is decrease.
- Envelope Length (CC13) (custom):  
	Sets the length of the envelope. 0 disables the envelope and makes volume constant. A high number is a long envelope, a low number is a short envelope.
- Sound Length Enable (CC14) (custom):  
	Sets whether the sound length feature is enabled or disabled. 0 is disabled, 127 is enabled.
- Sound Length (CC15) (custom):  
	Sets the length before the channel is automatically silenced. The lower the value, the longer the time before the channel is silenced.
- Sweep Speed (CC16) (custom):  
	Sets the speed of a sweep (singular pitch slide up or down).
- Sweep Shift (CC17) (custom):  
	Sets how much the pitch changes each "step".
- Sweep Direction (CC18) (custom):  
	Sets whether a sweep should cause the pitch to increase or decrease.
- Duty Cycle (CC19) (custom):  
	Sets the shape of a square's waveform.
- Noise Length (CC20) (custom):  
	Sets whether the noise should be long (percussive) or short (slightly melodic). 0 is long and 127 is short.
- Wave Index Selector (CC21 and CC53) (custom):  
	A 14-bit combined CC. CC21 is the MSB and CC53 is the LSB. Sets the waveform to use for the wave channel from the list given in a sysex message. A midi file can have up to 16383 waves.

## Usage Tips

### Disable Midi Reset on Playback Start, Stop, and Skip in your DAW

Nelly doesn't have a user interface, it relies on Midi messages to set all of its settings. Because of this, it is important to disable any Midi Reset features in a DAW so that the midi messages sent to Nelly match up with the Midi messages you are sending.

<!--
If you do want to do a Midi Reset, please use the GM Reset universal sysex:  
```
F0 7E 7F 09 01 F7
```
-->

### How to Disable Attack on Notes / Play Legato

If you want to smoothly play many notes in succession, without resetting the envelope, as if all of the notes were a single note with pitch bend, this tip will be helpful.

Set the velocity of all but the first note in the legato sequence to less than half (velocity < 64); this disables re-triggering the Game Boy channel on a note on. Then, select the note ends of every note in the legato sequence and extend them slightly so that the notes overlap each other; this plugin usually silences the Game Boy channel during a Midi Note Off event, overlapping the notes will disable that silencing, the overlapping notes will not play two notes at once because each channel of the Game Boy is monophonic.

### I Recommend Reading Game Boy APU Documentation

Because this plugin converts midi events to Game Boy APU register writes, understanding how the Game Boy APU functions will be very helpful for making music using this plugin. Please read [Pan Docs' section on Game Boy Audio](https://gbdev.io/pandocs/Audio.html).

### You May Hear Loud Notes When Seeking Through a Song

After a midi note off event, the plugin will silence that channel; however, if you seek to a large gap in between two notes, the plugin may "forget" that the channel should be silent right now and play a very loud note.  

To work around this, it is recommended to manually silence the channel for long gaps in notes. You can manually silence a channel by setting volume (CC07) to 0; for all channels except wave, you will also need to set envelope direction (CC12) to 127 (to prevent an audio pop) and insert a short note (to trigger the channel so that the volume change is read).

(This may no longer be an issue in v3.0.0)

<!--
### Close Notes Silencing Each Other

If, when editing the song, you notice that notes right next to eachother seem to be silencing eachother, try zooming in very closely; you'll likely see a very small overlap between the two notes. Remove this overlap so the notes will play properly.
-->

### If Playback is Paused, the Wave Channel Will Be Inaudible On the Piano Roll

This happens because this plugin resets its internal emulated APU whenever playback is paused. This is necessary in order to make sure that loud sounds do not continously play when playback is paused, but it also clears wave data from the APU's memory (though the wave data is still stored in the plugin's memory, and the correct wave will be played when resuming the song).

I work around this by placing my notes on channel 1 or 2, then moving those notes to channel 3. This workaround works best if you also have each midi channel on a separate track in your DAW.

### Notes Don't End When They Should When Using CLAP Plugin

I've noticed that the CLAP version of the plugin does not properly end the note when the note off message has a velocity of 0 or undefined. To fix this, please edit your midi to set all note off velocity values to 127. 

### Other

[Please do not attempt to use this plugin in FL Studio](https://gist.github.com/Thysbelon/a69da7038e65023a29168d9ef449acda).

## Information For Software Developers

The clap folder in src should contain the contents of [the clap repository's include folder](https://github.com/free-audio/clap/tree/main/include/clap).

The last time I worked on the CLAP version of this plugin, I used [CLAP commit 69a6925](https://github.com/free-audio/clap/tree/69a69252fdd6ac1d06e246d9a04c0a89d9607a17).

## Credits

- This plugin uses [Furnace](https://github.com/tildearrow/furnace)'s Game Boy APU emulation core, which is itself derived from [SameBoy](https://github.com/LIJI32/SameBoy)'s APU emulation code.
