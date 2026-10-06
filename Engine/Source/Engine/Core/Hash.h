#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Non-cryptographic hashes (Architecture §4.8, §6.8, §7.5). Every function is pure, deterministic across platforms,
// compilers and configurations, and safe to call from any thread. XXH64 is an in-house implementation of the xxHash64
// specification, verified against the reference test vectors; its values are persisted (sub-asset handles, prefab
// instance IDs, cache keys, cooked payload checksums, state hashes), so the output for given bytes and seed never
// changes.

namespace Engine {

	// XXH64 of `data` with `seed`.
	[[nodiscard]] uint64_t XXH64(std::span<const std::byte> data, uint64_t seed = 0);

	// XXH64 of the bytes of `text` (no terminator) with `seed`.
	[[nodiscard]] uint64_t XXH64(std::string_view text, uint64_t seed = 0);

	// Streaming XXH64: Update calls in sequence give the same digest as one XXH64 call over the concatenated bytes. A
	// value type; not thread-safe (one owner).
	class XXH64Hasher
	{
	public:
		explicit XXH64Hasher(uint64_t seed = 0);

		// Restarts with `seed`, discarding everything hashed so far.
		void Reset(uint64_t seed = 0);

		void Update(std::span<const std::byte> data);
		void Update(std::string_view text);
		// The 8 little-endian bytes of `value`.
		void UpdateU64(uint64_t value);

		// The digest of everything hashed since construction or Reset. Does not change the state: more Update calls may
		// follow.
		[[nodiscard]] uint64_t Digest() const;
	};

	// Hash64(seed, value) = XXH64(8 little-endian bytes of value, seed). Combines two 64-bit identities: runtime UUIDs
	// (Hash64(sessionSeed, spawnCounter)) and prefab instance IDs (Hash64(instanceRootID, prefabEntityID)), §4.8.
	[[nodiscard]] uint64_t Hash64(uint64_t seed, uint64_t value);

	// Hash64(seed, key) = XXH64(UTF-8 bytes of key, seed). Sub-asset handles: Hash64(sourceHandle, subAssetKey), §7.1.
	[[nodiscard]] uint64_t Hash64(uint64_t seed, std::string_view key);

	// 32-bit FNV-1a (offset basis 0x811c9dc5, prime 0x01000193) of the bytes of `text`.
	[[nodiscard]] constexpr uint32_t FNV1a32(std::string_view text)
	{
		uint32_t hash = 0x811c9dc5u;
		for (const char character : text)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 0x01000193u;
		}
		return hash;
	}

	// 64-bit FNV-1a (offset basis 0xcbf29ce484222325, prime 0x100000001b3) of the bytes of `text`.
	[[nodiscard]] constexpr uint64_t FNV1a64(std::string_view text)
	{
		uint64_t hash = 0xcbf29ce484222325ull;
		for (const char character : text)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

}
