#include "event_source.h"

namespace andromeda::editor {

const EventSource* EventSources::get_source(EventSourceKind source_kind) {
	static_assert(sources.size() == static_cast<std::size_t>(EventSourceKind::Count));

	EventSource* src = nullptr;
	for (auto& s : sources) {
		if (s.kind != source_kind) continue;
		src = &s;
		break;
	}
	return src;
}

std::string_view EventSources::cc_name_of(std::uint8_t controller) {
	if (control_change_names.empty()) {
		control_change_names.resize(128, "");

		auto& ccn = control_change_names;
		ccn[0] = "Bank Select";
		ccn[1] = "Mod Wheel";
		ccn[2] = "Breath";

		ccn[4] = "Foot";
		ccn[5] = "Porta Time";
		ccn[6] = "Data Entry";
		ccn[7] = "Volume";
		ccn[8] = "Balance";
		
		ccn[10] = "Pan";
		ccn[11] = "Expression";
		ccn[12] = "Effect 1";
		ccn[13] = "Effect 2";

		ccn[16] = "General Purpose 1";
		ccn[17] = "General Purpose 2";
		ccn[18] = "General Purpose 3";
		ccn[19] = "General Purpose 4";

		ccn[32] = "Bank Select (LSB)";
		ccn[33] = "Mod Wheel (LSB)";
		ccn[34] = "Breath (LSB)";

		ccn[36] = "Foot (LSB)";
		ccn[37] = "Porta Time (LSB)";
		ccn[38] = "Data Entry (LSB)";
		ccn[39] = "Volume (LSB)";
		ccn[40] = "Balance (LSB)";

		ccn[42] = "Pan (LSB)";
		ccn[43] = "Expression (LSB)";
		ccn[44] = "Effect 1 (LSB)";
		ccn[45] = "Effect 2 (LSB)";

		ccn[48] = "General Purpose 1 (LSB)";
		ccn[49] = "General Purpose 2 (LSB)";
		ccn[50] = "General Purpose 3 (LSB)";
		ccn[51] = "General Purpose 4 (LSB)";

		ccn[64] = "Sustain";
		ccn[65] = "Portamento";
		ccn[66] = "Sostenuto";
		ccn[67] = "Soft Pedal";
		ccn[68] = "Legato";
		ccn[69] = "Hold 2";
		ccn[70] = "Sound var.";

		ccn[71] = "Resonance (timbre)";
		ccn[72] = "Release";
		ccn[73] = "Attack";
		ccn[74] = "Brightness (cutoff)";
		ccn[75] = "Decay";
		ccn[76] = "Vibrato rate";
		ccn[77] = "Vibrato depth";
		ccn[78] = "Vibrato delay";
		ccn[79] = "Sound controller 10";

		ccn[80] = "General Purpose 5";
		ccn[81] = "General Purpose 6";
		ccn[82] = "General Purpose 7";
		ccn[83] = "General Purpose 8";
		ccn[84] = "Portamento Control";
		ccn[88] = "HD Velocity";

		ccn[91] = "Reverb";
		ccn[92] = "Tremolo";
		ccn[93] = "Chorus";
		ccn[94] = "Detune";
		ccn[95] = "Phaser depth";
		ccn[96] = "Data increment";
		ccn[97] = "Data decrement";

		ccn[98] =  "NRPN (LSB)";
		ccn[99] =  "NRPN (MSB)";
		ccn[100] = "RPN (LSB)";
		ccn[101] = "RPN (MSB)";

		ccn[120] = "All sound off";
		ccn[121] = "Reset all cotrollers";
		ccn[122] = "Local control";
		ccn[123] = "All notes off";
		ccn[124] = "Omni mode off";
		ccn[125] = "Omni mode on";
		ccn[126] = "Mono mode on";
		ccn[127] = "Poly mode on";
	}

	if (controller >= control_change_names.size()) return "Unknown";
	return control_change_names[controller];
}

std::string_view EventSources::pc_name_of(std::uint8_t program) {
	if (patch_names.empty()) {
		patch_names.resize(128, "");

		auto& pcn = patch_names;
		// Pianos
		pcn[0] = "Acoustic grand piano";
		pcn[1] = "Bright acoustic piano";
		pcn[2] = "Electric grand piano";
		pcn[3] = "Honky-tonk piano";
		pcn[4] = "Electric piano 1";
		pcn[5] = "Electric piano 2";
		pcn[6] = "Harpsichord";
		pcn[7] = "Clavinet";

		// Chormatic percussion
		pcn[8] = "Celesta";
		pcn[9] = "Glockenspiel";
		pcn[10] = "Music box";
		pcn[11] = "Vibraphone";
		pcn[12] = "Marimba";
		pcn[13] = "Xylophone";
		pcn[14] = "Tubular bells";
		pcn[15] = "Dulcimer";

		// Organs
		pcn[16] = "Drawbar organ";
		pcn[17] = "Percussive organ";
		pcn[18] = "Rock organ";
		pcn[19] = "Church organ";
		pcn[20] = "Reed organ";
		pcn[21] = "Accordion";
		pcn[22] = "Harmonica";
		pcn[23] = "Tango Accordion";

		// Guitars
		pcn[24] = "Acoustic Guitar (nylon)";
		pcn[25] = "Acoustic Guitar (steel)";
		pcn[26] = "Electric Guitar (jazz)";
		pcn[27] = "Electric Guitar (clean)";
		pcn[28] = "Electric Guitar (muted)";
		pcn[29] = "Overdriven Guitar";
		pcn[30] = "Distortion Guitar";
		pcn[31] = "Guitar harmonics";

		// Basses
		pcn[32] = "Acoustic Bass";
		pcn[33] = "Electric Bass (finger)";
		pcn[34] = "Electric Bass (pick)";
		pcn[35] = "Fretless Bass";
		pcn[36] = "Slap Bass 1";
		pcn[37] = "Slap Bass 2";
		pcn[38] = "Synth Bass 1";
		pcn[39] = "Synth Bass 2";

		// Strings
		pcn[40] = "Violin";
		pcn[41] = "Viola";
		pcn[42] = "Cello";
		pcn[43] = "Contrabass";
		pcn[44] = "Tremolo Strings";
		pcn[45] = "Pizzicato Strings";
		pcn[46] = "Orchestral Harp";
		pcn[47] = "Timpani";

		// Ensemble
		pcn[48] = "String Ensemble 1";
		pcn[49] = "String Ensemble 2";
		pcn[50] = "SynthStrings 1";
		pcn[51] = "SynthStrings 2";
		pcn[52] = "Choir Aahs";
		pcn[53] = "Voice Oohs";
		pcn[54] = "Synth Voice";
		pcn[55] = "Orchestra Hit";

		// Brass
		pcn[56] = "Trumpet";
		pcn[57] = "Trombone";
		pcn[58] = "Tuba";
		pcn[59] = "Muted Trumpet";
		pcn[60] = "French Horn";
		pcn[61] = "Brass Section";
		pcn[62] = "SynthBrass 1";
		pcn[63] = "SynthBrass 2";

		// Reed
		pcn[64] = "Soprano Sax";
		pcn[65] = "Alto Sax";
		pcn[66] = "Tenor Sax";
		pcn[67] = "Baritone Sax";
		pcn[68] = "Oboe";
		pcn[69] = "English Horn";
		pcn[70] = "Bassoon";
		pcn[71] = "Clarinet";

		// Pipe
		pcn[72] = "Piccolo";
		pcn[73] = "Flute";
		pcn[74] = "Recorder";
		pcn[75] = "Pan Flute";
		pcn[76] = "Blown Bottle";
		pcn[77] = "Shakuhachi";
		pcn[78] = "Whistle";
		pcn[79] = "Ocarina";

		// Synth Lead
		pcn[80] = "Lead 1 (square)";
		pcn[81] = "Lead 2 (sawtooth)";
		pcn[82] = "Lead 3 (calliope)";
		pcn[83] = "Lead 4 (chiff)";
		pcn[84] = "Lead 5 (charang)";
		pcn[85] = "Lead 6 (voice)";
		pcn[86] = "Lead 7 (fifths)";
		pcn[87] = "Lead 8 (bass + lead)";

		// Synth Pad
		pcn[88] = "Pad 1 (new age)";
		pcn[89] = "Pad 2 (warm)";
		pcn[90] = "Pad 3 (polysynth)";
		pcn[91] = "Pad 4 (choir)";
		pcn[92] = "Pad 5 (bowed)";
		pcn[93] = "Pad 6 (metallic)";
		pcn[94] = "Pad 7 (halo)";
		pcn[95] = "Pad 8 (sweep)";

		// Synth Effects
		pcn[96] = "FX 1 (rain)";
		pcn[97] = "FX 2 (soundtrack)";
		pcn[98] = "FX 3 (crystal)";
		pcn[99] = "FX 4 (atmosphere)";
		pcn[100] = "FX 5 (brightness)";
		pcn[101] = "FX 6 (goblins)";
		pcn[102] = "FX 7 (echoes)";
		pcn[103] = "FX 8 (sci-fi)";

		// Ethnic
		pcn[104] = "Sitar";
		pcn[105] = "Banjo";
		pcn[106] = "Shamisen";
		pcn[107] = "Koto";
		pcn[108] = "Kalimba";
		pcn[109] = "Bag pipe";
		pcn[110] = "Fiddle";
		pcn[111] = "Shanai";

		// Percussive
		pcn[112] = "Tinkle Bell";
		pcn[113] = "Agogo";
		pcn[114] = "Steel Drums";
		pcn[115] = "Woodblock";
		pcn[116] = "Taiko Drum";
		pcn[117] = "Melodic Tom";
		pcn[118] = "Synth Drum";
		pcn[119] = "Reverse Cymbal";

		// Sound Effects
		pcn[120] = "Guitar Fret Noise";
		pcn[121] = "Breath Noise";
		pcn[122] = "Seashore";
		pcn[123] = "Bird Tweet";
		pcn[124] = "Telephone Ring";
		pcn[125] = "Helicopter";
		pcn[126] = "Applause";
		pcn[127] = "Gunshot";
	}

	if (program >= patch_names.size()) return "Unknown";
	return patch_names[program];
}

}