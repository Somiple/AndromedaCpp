#include <yaml-cpp/yaml.h>
#include "editor_config.h"
#include <fstream>
#include <iostream>
#include "core/debugger.h"

namespace {
using andromeda::util::Debugger;

template <typename T>
void load_val(const YAML::Node& node, std::string key, T& val) {
	if (node[key]) val = node[key].as<T>();
}

}

namespace andromeda::config {

void EditorConfig::load_config_or_default() {
	try {
		std::ifstream stream("./config.yml", std::ios::in);
		if (!stream.is_open()) {
			Debugger::log_warning("Failed to load config.yml. Using default config");
			return;
		}

		if (const YAML::Node root = YAML::Load(stream)) {
			if (const YAML::Node midi_section = root["midi"]) {
				ConfigMIDI midi{};

				if (const YAML::Node import_section = midi_section["import"]) {
					ConfigMIDI::Import import{};
					load_val(import_section, "discard_empty_tracks", import.discard_empty_tracks);
					load_val(import_section, "keep_empty_with_cc", import.keep_empty_with_cc);
					load_val(import_section, "reassign_channel_10", import.reassign_channel_10);
					load_val(import_section, "remove_overlaps", import.remove_overlaps);
					midi.import_settings = std::move(import);
				}

				if (const YAML::Node export_section = midi_section["export"]) {
					ConfigMIDI::Export export_{};
					load_val(export_section, "discard_empty_tracks", export_.discard_empty_tracks);
					load_val(export_section, "ignore_muted_tracks", export_.ignore_muted_tracks);
					midi.export_settings = std::move(export_);
				}
				
				this->midi = std::move(midi);
			}

			if (const YAML::Node audio_section = root["audio"]) {
				ConfigAudio audio{};
				load_val(audio_section, "audio_engine", audio.audio_engine_id);
				this->audio = std::move(audio);
			}
		}
	} catch (const YAML::Exception& e) {
		Debugger::log_error(std::format("Failed to load config. Using default config. \n{}", e.what()));
	}
}

void EditorConfig::save_config() const {
	YAML::Node config;
	config["midi"]["import"]["discard_empty_tracks"] = midi.import_settings.discard_empty_tracks;
	config["midi"]["import"]["keep_empty_with_cc"] = midi.import_settings.keep_empty_with_cc;
	config["midi"]["import"]["reassign_channel_10"] = midi.import_settings.reassign_channel_10;
	config["midi"]["import"]["remove_overlaps"] = midi.import_settings.remove_overlaps;

	config["midi"]["export"]["discard_empty_tracks"] = midi.export_settings.discard_empty_tracks;
	config["midi"]["export"]["ignore_muted_tracks"] = midi.export_settings.ignore_muted_tracks;
	
	config["audio"]["audio_engine"] = audio.audio_engine_id;
	std::ofstream file("./config.yml");
	file << config;
}

}