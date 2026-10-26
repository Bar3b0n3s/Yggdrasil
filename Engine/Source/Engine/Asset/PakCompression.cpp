#include "EnginePCH.h"
#include "Engine/Asset/PakCompression.h"

#include "Engine/Core/FatalError.h"

#include <stb_image.h>

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string_view>

// stb_image_write.h declares its deflate only in its implementation part, which Graphics/ThirdParty/
// StbImageImplementation.cpp compiles with C linkage (STBIWDEF is extern "C" in C++); this declares that definition, so no
// vendored file changes (Docs/Decisions/0030-m19-contract.md decision 17). It returns a zlib stream allocated with malloc
// (STBIW_MALLOC), or null when it cannot allocate its hash table.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int dataLength, int* outLength, int quality);

namespace Engine {

	namespace {

		// Frees what stb_image_write allocates (STBIW_FREE is free).
		struct StbWriteDeleter
		{
			void operator()(unsigned char* bytes) const { std::free(bytes); }
		};

		// The most a zlib stream inflates: every Huffman symbol takes at least one bit, and a length and distance pair,
		// at least two bits, copies at most 258 bytes, so N stream bytes inflate to at most 1032 * N (the bound
		// Graphics/Image.cpp applies to PNG data). A larger claimed size is refused before it is allocated.
		constexpr uint64_t MaxInflateRatio = 1032;

		// The largest input stb's deflate takes without overflowing: it grows its output buffer with int arithmetic (a
		// capacity of 2m + 1 bytes), which overflows once the output passes 1,610,612,735 bytes, and its fixed-Huffman
		// output is at most nine bits per input byte before its stored fallback. A 1 GiB input stays below that bound with
		// room to spare; a larger one is refused, and PakWriter stores it.
		constexpr uint64_t MaxStbDeflateInputSize = uint64_t{ 1 } << 30;
		static_assert(MaxStbDeflateInputSize <= MaxPakCompressedEntrySize, "stb's deflate limit is the tighter one");

		// The zlib stream of no bytes, with stb's header (78 5e): one final fixed-Huffman block holding only its end code
		// (03 00), then the Adler-32 of nothing (00 00 00 01). stb's deflate writes no block at all for empty input (its
		// stored fallback loops over no data), which no inflater reads, so DeflatePakEntry returns this stream instead.
		constexpr std::array<std::byte, 8> EmptyZlibStream = { std::byte{ 0x78 }, std::byte{ 0x5e }, std::byte{ 0x03 }, std::byte{ 0x00 },
			std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x01 } };

	}

	namespace Utils {

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		static std::string_view GetStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

	}

	Result<Buffer> DeflatePakEntry(std::span<const std::byte> bytes)
	{
		if (bytes.size() > MaxStbDeflateInputSize)
			return MakeError(ErrorCode::InvalidArgument, "cannot deflate a pak entry of {} bytes: stb's deflate takes at most {}", bytes.size(), MaxStbDeflateInputSize);
		if (bytes.empty())
			return Buffer(EmptyZlibStream.begin(), EmptyZlibStream.end());

		// stbi_zlib_compress is not const-correct: it only reads its input.
		auto* data = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(bytes.data()));
		int length = 0;
		const std::unique_ptr<unsigned char, StbWriteDeleter> stream(stbi_zlib_compress(data, static_cast<int>(bytes.size()), &length, PakDeflateQuality));
		if (stream == nullptr)
			FatalError(FatalErrorKind::OutOfMemory, std::format("out of memory while deflating a pak entry of {} bytes", bytes.size()));
		const auto* first = reinterpret_cast<const std::byte*>(stream.get());
		return Buffer(first, first + length);
	}

	Result<Buffer> InflatePakEntry(std::span<const std::byte> stored, uint64_t size)
	{
		if (stored.size() > MaxPakCompressedEntrySize || size > MaxPakCompressedEntrySize)
		{
			return MakeError(ErrorCode::Validation, "cannot inflate {} bytes into {}: the pak entry codec takes at most {}", stored.size(), size,
				MaxPakCompressedEntrySize);
		}
		if (stored.empty())
			return MakeError(ErrorCode::Validation, "cannot inflate an empty stream into {} bytes", size);
		if (size > MaxInflateRatio * stored.size())
		{
			return MakeError(ErrorCode::Validation, "a zlib stream of {} bytes inflates to at most {}, not {}", stored.size(),
				MaxInflateRatio * stored.size(), size);
		}

		// Inflated into exactly `size` bytes: stb refuses a stream that holds more (a fixed buffer does not grow). An empty
		// result still gets a valid buffer address.
		Buffer inflated(static_cast<size_t>(size));
		std::byte empty{};
		char* destination = reinterpret_cast<char*>(size == 0 ? &empty : inflated.data());
		const int written = stbi_zlib_decode_buffer(destination, static_cast<int>(size), reinterpret_cast<const char*>(stored.data()),
			static_cast<int>(stored.size()));
		if (written < 0)
			return MakeError(ErrorCode::Validation, "the zlib stream of {} bytes does not inflate: {}", stored.size(), Utils::GetStbFailureReason());
		if (static_cast<uint64_t>(written) != size)
			return MakeError(ErrorCode::Validation, "the zlib stream of {} bytes inflates to {} bytes, not {}", stored.size(), written, size);
		return inflated;
	}

}
