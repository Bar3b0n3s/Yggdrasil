#include "EnginePCH.h"
#include "Engine/Core/Log.h"

// M1 contract stub (Roadmap rule 3): stream A implements the loggers, sinks, the fallback logger and listeners. Until
// then every channel writes to one logger without sinks, so log statements compile and run but print nothing.
// LogContextScope (LogContext.h) is implemented here because it is trivial and other streams rely on it.

namespace Engine {

	static thread_local LogContext s_CurrentLogContext;

	LogContextScope::LogContextScope(const LogContext& context)
		: m_Previous(s_CurrentLogContext)
	{
		s_CurrentLogContext = context;
	}

	LogContextScope::~LogContextScope()
	{
		s_CurrentLogContext = m_Previous;
	}

	const LogContext& LogContextScope::GetCurrent()
	{
		return s_CurrentLogContext;
	}

	Status Log::Initialize(const LogSpecification& /*specification*/)
	{
		return MakeError(ErrorCode::Unsupported, "Log::Initialize is an M1 contract stub");
	}

	void Log::Shutdown()
	{
	}

	bool Log::IsInitialized()
	{
		return false;
	}

	void Log::Flush()
	{
	}

	spdlog::logger& Log::GetLogger(LogChannel /*channel*/)
	{
		static spdlog::logger s_StubLogger("Stub");
		return s_StubLogger;
	}

	RingBufferSink& Log::GetRingBuffer()
	{
		static RingBufferSink s_StubRingBuffer(1);
		return s_StubRingBuffer;
	}

	uint64_t Log::AddListener(LogListener /*listener*/)
	{
		return 1;
	}

	void Log::RemoveListener(uint64_t /*listenerID*/)
	{
	}

}
