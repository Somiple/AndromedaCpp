#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::editor::codec {

using Bytes = std::vector<std::uint8_t>;

template <typename T>
using Result = std::expected<T, std::string>;

// Upper bound for a single block (raw or compressed). Protects the reader from
// allocating gigabytes because of a corrupt size field.
inline constexpr std::size_t MAX_BLOCK_SIZE = 512u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Payload encoding (uncompressed). Everything is delta encoded on tick, and
// every integer that can be large (deltas, note lengths, meta data sizes) is a
// MIDI-style VLQ (big-endian 7-bit groups, high bit = "more bytes follow").
//
//   note:          vlq(delta start) vlq(length) u8 key u8 velocity u8 channel
//   channel event: vlq(delta tick)  u8 (tag << 4 | channel)  1-2 data bytes
//   meta event:    vlq(delta tick)  u8 type  vlq(data size)   data bytes
//
// The encoders never require sorted input: they sort a copy if needed.
// The decoders read until the payload is exhausted; if `expected_count` is
// given, a mismatch is reported as corruption.
// ---------------------------------------------------------------------------
Bytes encode_notes(std::span<const midi::Note> notes);
Bytes encode_channel_events(std::span<const midi::ChannelEvent> events);
Bytes encode_meta_events(std::span<const midi::MetaEvent> events);

Result<std::vector<midi::Note>> decode_notes(
    std::span<const std::uint8_t> bytes, std::optional<std::size_t> expected_count = std::nullopt);
Result<std::vector<midi::ChannelEvent>> decode_channel_events(
    std::span<const std::uint8_t> bytes, std::optional<std::size_t> expected_count = std::nullopt);
Result<std::vector<midi::MetaEvent>> decode_meta_events(
    std::span<const std::uint8_t> bytes, std::optional<std::size_t> expected_count = std::nullopt);

// ---------------------------------------------------------------------------
// Compression (zstd). Swapping the algorithm only means changing these two.
// An empty input compresses to an empty output (and back).
// ---------------------------------------------------------------------------
Result<Bytes> compress(std::span<const std::uint8_t> raw);
Result<Bytes> decompress(std::span<const std::uint8_t> compressed, std::size_t raw_size);

}
