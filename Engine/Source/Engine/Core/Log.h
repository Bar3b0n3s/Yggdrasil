#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/LogContext.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/RingBufferSink.h"

#include <spdlog/logger.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// Logging (Architecture §4.4, CodeStyle §11). This is the only public header that includes spdlog (§3 rule 3).
//
//   Engine code:  ENGINE_CORE_TRACE / INFO / WARN / ERROR / CRITICAL   (Engine logger)
//   Client code:  ENGINE_TRACE / INFO / WARN / ERROR / CRITICAL        (App logger; Editor and Runtime)
//
// Messages are std::format strings checked at compile time, in sentence case without a trailing period. Trace is
// compiled out in Dist (the arguments are type-checked but never evaluated); Info and above are kept in every
// configuration. Nothing logs at Trace or Info every frame. The Script logger is written by the script engine through
// Log::GetLogger(LogChannel::Script).

namespace Engine {

	// Rotating log file: 5 files of 10 MB (Architecture §4.4).
	inline constexpr size_t LogFileMaxBytes = 10 * 1024 * 1024;
	inline constexpr size_t LogFileCount = 5;

	struct LogSpecification
	{
		// The rotating file sink's path, normally <UserData>/<AppName>/Logs/<exe>.log, computed by the caller (Platform
		// Paths, M2). Empty: no file sink.
		std::filesystem::path FilePath;
		// The colour console sink, which writes to stderr (stdout stays free for machine-readable output such as
		// --json). Ignored in Dist, which never has a console sink.
		bool Console = true;
		// Entries below this level are not printed to the console; the ring buffer and the file still receive them.
		LogLevel ConsoleLevel = LogLevel::Trace;
		size_t RingBufferCapacity = RingBufferSink::DefaultCapacity;
	};

	// Receives every entry appended to the ring buffer, synchronously on the logging thread, after it was stored, while
	// the listener registry is locked shared. It must not throw, and must not call AddListener or RemoveListener
	// (asserted). An entry logged from inside a listener (an assertion report, for example) is stored in the ring
	// buffer and printed, but not passed to the listeners again, so a listener never recurses or deadlocks.
	using LogListener = std::function<void(const LogEntry& entry)>;

	// The logger registry: process-level state (Architecture §3 rule 5), initialized once by ProcessContext or the Tests
	// main and shut down last. Every member is thread-safe, with one ordering rule: Initialize and Shutdown replace the
	// loggers and the ring buffer, so they run while no other thread logs (at process start and end, before the first
	// and after the last worker thread).
	class Log
	{
	public:
		Log() = delete;

		// Creates the Engine, App and Script loggers and their sinks: the ring buffer, the console (unless Dist or
		// disabled) and the rotating file (when FilePath is set; its directory is created). Errors: Io when the log file
		// or its directory cannot be created; no logger is replaced then. Calling it while initialized is a programmer
		// error (asserted).
		[[nodiscard]] static Status Initialize(const LogSpecification& specification);

		// Flushes and destroys every sink. Idempotent. Before Initialize and after Shutdown, entries at Warn and above
		// are written to stderr by a fallback logger and everything else is discarded; nothing crashes.
		static void Shutdown();

		[[nodiscard]] static bool IsInitialized();

		// Flushes the console and file sinks (FatalError and crash paths call it).
		static void Flush();

		// The spdlog logger of `channel`: after Initialize the registered one, otherwise the fallback logger. The reference
		// stays valid until the next Initialize or Shutdown.
		[[nodiscard]] static spdlog::logger& GetLogger(LogChannel channel);

		// The ring buffer of the current initialization. Before Initialize and after Shutdown, an empty buffer that
		// receives nothing.
		[[nodiscard]] static RingBufferSink& GetRingBuffer();

		// Registers a listener for every subsequently stored entry and returns its ID (never 0). Listeners run in
		// registration order. They belong to the process-wide registry, so they stay registered across Shutdown and
		// Initialize, and receive entries only while the log is initialized. Used by Test::ExpectLog; the editor console
		// polls the ring buffer instead.
		[[nodiscard]] static uint64_t AddListener(LogListener listener);

		// Unregisters a listener; unknown IDs are ignored. When it returns, the listener is not running and will not run
		// again.
		static void RemoveListener(uint64_t listenerID);
	};

	namespace Detail {

#if defined(ENGINE_DIST)
		inline constexpr bool LogTraceEnabled = false;
#else
		inline constexpr bool LogTraceEnabled = true;
#endif

		[[nodiscard]] constexpr spdlog::level::level_enum ToSpdlogLevel(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Trace:    return spdlog::level::trace;
				case LogLevel::Info:     return spdlog::level::info;
				case LogLevel::Warn:     return spdlog::level::warn;
				case LogLevel::Error:    return spdlog::level::err;
				case LogLevel::Critical: return spdlog::level::critical;
			}
			return spdlog::level::critical;
		}

		// The message is formatted here, and spdlog receives the finished text. spdlog takes std::format_string only when
		// the library defines __cpp_lib_format >= 202207L; Apple's libc++ does not, so spdlog's formatting overloads
		// expect a runtime std::string_view there. The level check keeps disabled entries from being formatted.
		template<typename... Args>
		void LogWrite(LogChannel channel, LogLevel level, const char* file, int line, const char* function,
			std::format_string<Args...> format, Args&&... args)
		{
			spdlog::logger& logger = Log::GetLogger(channel);
			const spdlog::level::level_enum spdlogLevel = ToSpdlogLevel(level);
			if (!logger.should_log(spdlogLevel))
				return;

			const std::string message = std::format(format, std::forward<Args>(args)...);
			logger.log(spdlog::source_loc{ file, line, function }, spdlogLevel, std::string_view(message));
		}

	}

}

#define ENGINE_LOG_IMPL(channel, level, ...) \
	::Engine::Detail::LogWrite(channel, level, __FILE__, __LINE__, static_cast<const char*>(__func__), __VA_ARGS__)

#define ENGINE_LOG_TRACE_IMPL(channel, ...) \
	do \
	{ \
		if constexpr (::Engine::Detail::LogTraceEnabled) \
			ENGINE_LOG_IMPL(channel, ::Engine::LogLevel::Trace, __VA_ARGS__); \
	} while (false)

#define ENGINE_CORE_TRACE(...)    ENGINE_LOG_TRACE_IMPL(::Engine::LogChannel::Engine, __VA_ARGS__)
#define ENGINE_CORE_INFO(...)     ENGINE_LOG_IMPL(::Engine::LogChannel::Engine, ::Engine::LogLevel::Info, __VA_ARGS__)
#define ENGINE_CORE_WARN(...)     ENGINE_LOG_IMPL(::Engine::LogChannel::Engine, ::Engine::LogLevel::Warn, __VA_ARGS__)
#define ENGINE_CORE_ERROR(...)    ENGINE_LOG_IMPL(::Engine::LogChannel::Engine, ::Engine::LogLevel::Error, __VA_ARGS__)
#define ENGINE_CORE_CRITICAL(...) ENGINE_LOG_IMPL(::Engine::LogChannel::Engine, ::Engine::LogLevel::Critical, __VA_ARGS__)

#define ENGINE_TRACE(...)    ENGINE_LOG_TRACE_IMPL(::Engine::LogChannel::App, __VA_ARGS__)
#define ENGINE_INFO(...)     ENGINE_LOG_IMPL(::Engine::LogChannel::App, ::Engine::LogLevel::Info, __VA_ARGS__)
#define ENGINE_WARN(...)     ENGINE_LOG_IMPL(::Engine::LogChannel::App, ::Engine::LogLevel::Warn, __VA_ARGS__)
#define ENGINE_ERROR(...)    ENGINE_LOG_IMPL(::Engine::LogChannel::App, ::Engine::LogLevel::Error, __VA_ARGS__)
#define ENGINE_CRITICAL(...) ENGINE_LOG_IMPL(::Engine::LogChannel::App, ::Engine::LogLevel::Critical, __VA_ARGS__)
