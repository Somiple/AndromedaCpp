#pragma once

#include <utility>

namespace andromeda::config {

struct EditorConfig {
	struct ConfigMIDI {
		struct Import {
			bool discard_empty_tracks = false;
			bool keep_empty_with_cc = true;
			bool reassign_channel_10 = false;
			bool remove_overlaps = false;
		};

		struct Export {
			bool discard_empty_tracks = true;
			bool ignore_muted_tracks = false;
			int threads = 0;
		};

		Import import_settings{};
		Export export_settings{};
	};

	struct ConfigAudio {
		int audio_engine_id = 0;
	};

	ConfigMIDI midi{};
	ConfigAudio audio{};

	void load_config_or_default();
	void save_config() const;
};

}