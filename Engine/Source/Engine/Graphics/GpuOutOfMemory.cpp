#include "EnginePCH.h"
#include "Engine/Graphics/GpuOutOfMemory.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <format>
#include <string>
#include <utility>

namespace Engine {

	namespace Utils {

		// The request's OutOfMemory error: the retry's message after the request's phrase, with the retry's contexts, location
		// and hint, so the request's caller sees which creation failed and where.
		[[nodiscard]] static Error MakeRequestOutOfMemoryError(std::string_view request, const Error& retried)
		{
			Error error = Error(ErrorCode::OutOfMemory,
				std::format("{}: out of GPU memory after trimming what could be rebuilt and retrying once: {}", request, retried.GetMessageText()))
							  .WithLocation(retried.GetLocation())
							  .WithHint(retried.GetHint());
			// Built into a new variable each time: an error is never moved onto itself (CodeStyle §7).
			for (const std::string& context : retried.GetContexts())
			{
				Error next = std::move(error).WithContext(context);
				error = std::move(next);
			}
			return error;
		}

	}

	bool IsGpuOutOfMemory(const Error& error, const GraphicsDevice& device)
	{
		return error.GetCode() == ErrorCode::Gpu && !device.IsLost();
	}

	Status RetryAfterTrim(GraphicsDevice& device, std::string_view request, const std::function<Status()>& attempt,
		const std::function<bool(const Error&)>& isOutOfMemory)
	{
		// A recovery's Release or Recreate: one attempt, its result unchanged. An out-of-memory creation there is a second
		// startup's, which fails the recovery, and the registry cannot trim while it runs (decision 42).
		if (device.GetRecreation().IsRunning())
			return attempt();

		const auto isRequestOutOfMemory = [&device, &isOutOfMemory](const Error& error)
		{
			return IsGpuOutOfMemory(error, device) && (!isOutOfMemory || isOutOfMemory(error));
		};
		Status first = attempt();
		if (first.has_value() || !isRequestOutOfMemory(first.error()))
			return first;

		ENGINE_CORE_WARN("{} ran out of GPU memory; trimming what can be rebuilt and retrying once: {}", request, first.error());
		device.TrimUnused();
		Status retried = attempt();
		if (retried.has_value() || !isRequestOutOfMemory(retried.error()))
			return retried;
		return std::unexpected(Utils::MakeRequestOutOfMemoryError(request, retried.error()));
	}

}
