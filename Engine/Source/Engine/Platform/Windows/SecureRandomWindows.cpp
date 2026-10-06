#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// SecureRandom::Fill on Windows: BCryptGenRandom with the system-preferred generator.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include <windows.h>
	#include <bcrypt.h>

namespace Engine {

	Status SecureRandom::Fill(std::span<std::byte> output)
	{
		// BCryptGenRandom takes a ULONG count, so a larger buffer is filled in pieces.
		constexpr size_t MaxChunk = std::numeric_limits<ULONG>::max();
		size_t offset = 0;
		while (offset < output.size())
		{
			const size_t chunk = std::min(output.size() - offset, MaxChunk);
			const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data() + offset), static_cast<ULONG>(chunk),
				BCRYPT_USE_SYSTEM_PREFERRED_RNG);
			if (!BCRYPT_SUCCESS(status))
				return MakeError(ErrorCode::Io, "BCryptGenRandom failed with NTSTATUS 0x{:08x}", static_cast<uint32_t>(status));
			offset += chunk;
		}
		return {};
	}

}

#endif
