#include "EnginePCH.h"
#include "Engine/Core/Hash.h"

#include <bit>

// XXH64 as specified by the xxHash project (doc/xxhash_spec.md, "XXH64 algorithm description"): 32-byte stripes feed
// four accumulators, the remaining bytes are mixed in 8, 4 and 1 at a time, and an avalanche step finishes. Input words
// are read little-endian byte by byte, so the result does not depend on the host's byte order or alignment.

namespace Engine {

	namespace Utils {

		static constexpr uint64_t Prime1 = 0x9E3779B185EBCA87ull;
		static constexpr uint64_t Prime2 = 0xC2B2AE3D27D4EB4Full;
		static constexpr uint64_t Prime3 = 0x165667B19E3779F9ull;
		static constexpr uint64_t Prime4 = 0x85EBCA77C2B2AE63ull;
		static constexpr uint64_t Prime5 = 0x27D4EB2F165667C5ull;

		static constexpr size_t StripeSize = 32;

		using Accumulators = std::array<uint64_t, 4>;

		static uint64_t ReadU64(std::span<const std::byte> bytes, size_t offset)
		{
			uint64_t value = 0;
			for (size_t index = 0; index < 8; ++index)
				value |= static_cast<uint64_t>(bytes[offset + index]) << (8 * index);
			return value;
		}

		static uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset)
		{
			uint32_t value = 0;
			for (size_t index = 0; index < 4; ++index)
				value |= static_cast<uint32_t>(bytes[offset + index]) << (8 * index);
			return value;
		}

		static uint64_t Round(uint64_t accumulator, uint64_t lane)
		{
			accumulator += lane * Prime2;
			accumulator = std::rotl(accumulator, 31);
			return accumulator * Prime1;
		}

		static uint64_t MergeAccumulator(uint64_t hash, uint64_t accumulator)
		{
			hash ^= Round(0, accumulator);
			return hash * Prime1 + Prime4;
		}

		static Accumulators InitialAccumulators(uint64_t seed)
		{
			return { seed + Prime1 + Prime2, seed + Prime2, seed, seed - Prime1 };
		}

		// The accumulators after one 32-byte stripe.
		static Accumulators ConsumeStripe(Accumulators accumulators, std::span<const std::byte> stripe)
		{
			for (size_t lane = 0; lane < accumulators.size(); ++lane)
				accumulators[lane] = Round(accumulators[lane], ReadU64(stripe, 8 * lane));
			return accumulators;
		}

		// The digest of `totalLength` bytes: `accumulators` hold every complete stripe (unused when totalLength < 32) and
		// `tail` the fewer than 32 bytes after them.
		static uint64_t Finish(const Accumulators& accumulators, uint64_t seed, uint64_t totalLength, std::span<const std::byte> tail)
		{
			uint64_t hash = 0;
			if (totalLength >= StripeSize)
			{
				hash = std::rotl(accumulators[0], 1) + std::rotl(accumulators[1], 7) + std::rotl(accumulators[2], 12)
					+ std::rotl(accumulators[3], 18);
				for (const uint64_t accumulator : accumulators)
					hash = MergeAccumulator(hash, accumulator);
			}
			else
			{
				hash = seed + Prime5;
			}
			hash += totalLength;

			size_t offset = 0;
			for (; offset + 8 <= tail.size(); offset += 8)
			{
				hash ^= Round(0, ReadU64(tail, offset));
				hash = std::rotl(hash, 27) * Prime1 + Prime4;
			}
			if (offset + 4 <= tail.size())
			{
				hash ^= static_cast<uint64_t>(ReadU32(tail, offset)) * Prime1;
				hash = std::rotl(hash, 23) * Prime2 + Prime3;
				offset += 4;
			}
			for (; offset < tail.size(); ++offset)
			{
				hash ^= static_cast<uint64_t>(tail[offset]) * Prime5;
				hash = std::rotl(hash, 11) * Prime1;
			}

			hash ^= hash >> 33;
			hash *= Prime2;
			hash ^= hash >> 29;
			hash *= Prime3;
			hash ^= hash >> 32;
			return hash;
		}

		static std::span<const std::byte> TextBytes(std::string_view text)
		{
			return std::as_bytes(std::span<const char>(text.data(), text.size()));
		}

		static std::array<std::byte, 8> LittleEndianBytes(uint64_t value)
		{
			std::array<std::byte, 8> bytes{};
			for (size_t index = 0; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::byte>(value >> (8 * index));
			return bytes;
		}

	}

	uint64_t XXH64(std::span<const std::byte> data, uint64_t seed)
	{
		Utils::Accumulators accumulators = Utils::InitialAccumulators(seed);
		size_t offset = 0;
		for (; data.size() - offset >= Utils::StripeSize; offset += Utils::StripeSize)
			accumulators = Utils::ConsumeStripe(accumulators, data.subspan(offset, Utils::StripeSize));
		return Utils::Finish(accumulators, seed, data.size(), data.subspan(offset));
	}

	uint64_t XXH64(std::string_view text, uint64_t seed)
	{
		return XXH64(Utils::TextBytes(text), seed);
	}

	XXH64Hasher::XXH64Hasher(uint64_t seed)
	{
		Reset(seed);
	}

	void XXH64Hasher::Reset(uint64_t seed)
	{
		m_Accumulators = Utils::InitialAccumulators(seed);
		m_Buffer = {};
		m_Seed = seed;
		m_TotalLength = 0;
		m_BufferSize = 0;
	}

	void XXH64Hasher::Update(std::span<const std::byte> data)
	{
		m_TotalLength += data.size();

		// Complete a buffered stripe first.
		size_t offset = 0;
		if (m_BufferSize > 0)
		{
			offset = std::min(data.size(), m_Buffer.size() - m_BufferSize);
			std::ranges::copy(data.first(offset), std::span<std::byte>(m_Buffer).subspan(m_BufferSize).begin());
			m_BufferSize += offset;
			if (m_BufferSize < m_Buffer.size())
				return;
			m_Accumulators = Utils::ConsumeStripe(m_Accumulators, m_Buffer);
			m_BufferSize = 0;
		}

		for (; data.size() - offset >= Utils::StripeSize; offset += Utils::StripeSize)
			m_Accumulators = Utils::ConsumeStripe(m_Accumulators, data.subspan(offset, Utils::StripeSize));

		const std::span<const std::byte> rest = data.subspan(offset);
		std::ranges::copy(rest, m_Buffer.begin());
		m_BufferSize = rest.size();
	}

	void XXH64Hasher::Update(std::string_view text)
	{
		Update(Utils::TextBytes(text));
	}

	void XXH64Hasher::UpdateU64(uint64_t value)
	{
		// Update(the 8 little-endian bytes of value) without the generic span handling: state hashes feed every value
		// through here.
		m_TotalLength += 8;
		for (size_t index = 0; index < 8; ++index)
		{
			m_Buffer[m_BufferSize++] = static_cast<std::byte>(value >> (8 * index));
			if (m_BufferSize == m_Buffer.size())
			{
				m_Accumulators = Utils::ConsumeStripe(m_Accumulators, m_Buffer);
				m_BufferSize = 0;
			}
		}
	}

	uint64_t XXH64Hasher::Digest() const
	{
		return Utils::Finish(m_Accumulators, m_Seed, m_TotalLength, std::span<const std::byte>(m_Buffer).first(m_BufferSize));
	}

	uint64_t Hash64(uint64_t seed, uint64_t value)
	{
		return XXH64(Utils::LittleEndianBytes(value), seed);
	}

	uint64_t Hash64(uint64_t seed, std::string_view key)
	{
		return XXH64(Utils::TextBytes(key), seed);
	}

}
