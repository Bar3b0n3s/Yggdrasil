#include "EnginePCH.h"
#include "Engine/Audio/Private/MiniaudioSupport.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

#include <format>
#include <string>

namespace Engine {

	namespace Utils {

		// ma_log_callback_proc: one miniaudio message (which ends with a newline) at Trace.
		static void ForwardMiniaudioLog(void* /*userData*/, ma_uint32 level, const char* message)
		{
			std::string_view text = message != nullptr ? std::string_view(message) : std::string_view();
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.remove_suffix(1);
			ENGINE_CORE_TRACE("miniaudio ({}): {}", ma_log_level_to_string(level), text);
		}

	}

	std::string_view DescribeMiniaudioResult(ma_result result)
	{
		const char* description = ma_result_description(result);
		return description != nullptr ? std::string_view(description) : std::string_view("Unknown error");
	}

	Error MakeMiniaudioError(ma_result result, std::string_view action)
	{
		const bool unsupported = result == MA_NOT_IMPLEMENTED || result == MA_NO_BACKEND || result == MA_FORMAT_NOT_SUPPORTED
			|| result == MA_DEVICE_TYPE_NOT_SUPPORTED || result == MA_SHARE_MODE_NOT_SUPPORTED;
		return Error(unsupported ? ErrorCode::Unsupported : ErrorCode::Io,
			std::format("{}: {} (miniaudio result {})", action, DescribeMiniaudioResult(result), static_cast<int>(result)));
	}

	MiniaudioLog::~MiniaudioLog()
	{
		if (m_Initialized)
			ma_log_uninit(&m_Log);
	}

	Status MiniaudioLog::Initialize()
	{
		ENGINE_CORE_ASSERT(!m_Initialized, "MiniaudioLog::Initialize called twice");
		if (const ma_result result = ma_log_init(nullptr, &m_Log); result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, "cannot create the miniaudio log"));
		m_Initialized = true;
		if (const ma_result result = ma_log_register_callback(&m_Log, ma_log_callback_init(&Utils::ForwardMiniaudioLog, nullptr));
			result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, "cannot register the miniaudio log callback"));
		return {};
	}

}
