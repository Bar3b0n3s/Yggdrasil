#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// SecureRandom::Fill on Linux (getrandom, which blocks only until the kernel's generator is seeded at boot) and macOS
// (arc4random_buf, which cannot fail).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#if defined(ENGINE_PLATFORM_LINUX)
		#include <sys/random.h>
	#elif defined(ENGINE_PLATFORM_MACOS)
		#include <stdlib.h>
	#endif

	#include <cerrno>
	#include <system_error>

namespace Engine {

	Status SecureRandom::Fill(std::span<std::byte> output)
	{
	#if defined(ENGINE_PLATFORM_LINUX)
		// getrandom may return fewer bytes than asked for large requests, or fail with EINTR when a signal arrives.
		size_t offset = 0;
		while (offset < output.size())
		{
			const ssize_t count = getrandom(output.data() + offset, output.size() - offset, 0);
			if (count < 0)
			{
				const int error = errno;
				if (error == EINTR)
					continue;
				return MakeError(ErrorCode::Io, "getrandom failed: {}", std::generic_category().message(error));
			}
			offset += static_cast<size_t>(count);
		}
	#elif defined(ENGINE_PLATFORM_MACOS)
		if (!output.empty())
			arc4random_buf(output.data(), output.size());
	#endif
		return {};
	}

}

#endif
