#include "EnginePCH.h"
#include "Engine/Core/Log.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Private/Immortal.h"
#include "Engine/Core/Private/NativePath.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

// Dist has no console sink (Architecture §4.4).
#if !defined(ENGINE_DIST)
	#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <system_error>

namespace Engine {

	static constexpr size_t LogChannelCount = 3;

	// spdlog patterns: the console shows the time, the logger, the coloured level and the message; the file adds the date
	// and the thread. The fallback logger has no colour (it is a plain stderr sink).
#if !defined(ENGINE_DIST)
	static constexpr const char* ConsolePattern = "[%H:%M:%S.%e] [%n] [%^%l%$] %v";
#endif
	static constexpr const char* FilePattern = "[%Y-%m-%d %H:%M:%S.%e] [%t] [%n] [%l] %v";
	static constexpr const char* FallbackPattern = "[%H:%M:%S.%e] [%n] [%l] %v";

	static thread_local LogContext s_CurrentLogContext;
	// Set while this thread runs the log listeners (see ListenerRegistry::Dispatch).
	static thread_local bool s_IsDispatchingToListeners = false;

	namespace Utils {

		static LogLevel FromSpdlogLevel(spdlog::level::level_enum level)
		{
			switch (level)
			{
				case spdlog::level::trace:
				case spdlog::level::debug:    return LogLevel::Trace;
				case spdlog::level::info:     return LogLevel::Info;
				case spdlog::level::warn:     return LogLevel::Warn;
				case spdlog::level::err:      return LogLevel::Error;
				case spdlog::level::critical:
				case spdlog::level::off:
				case spdlog::level::n_levels: return LogLevel::Critical;
			}
			return LogLevel::Critical;
		}

		static std::string DescribeErrno(int errorNumber)
		{
			if (errorNumber == 0)
				return {};
			return std::format(": {}", std::generic_category().message(errorNumber));
		}

	}

	namespace {

		struct ListenerRecord
		{
			uint64_t ID = 0;
			LogListener Callback;
		};

		// Marks the calling thread as running the log listeners for the lifetime of the scope.
		class ListenerDispatchScope
		{
		public:
			ListenerDispatchScope()
			{
				s_IsDispatchingToListeners = true;
			}

			~ListenerDispatchScope()
			{
				s_IsDispatchingToListeners = false;
			}

			ListenerDispatchScope(const ListenerDispatchScope&) = delete;
			ListenerDispatchScope& operator=(const ListenerDispatchScope&) = delete;
			ListenerDispatchScope(ListenerDispatchScope&&) = delete;
			ListenerDispatchScope& operator=(ListenerDispatchScope&&) = delete;
		};

		// The process-wide listener list behind Log::AddListener. It outlives every initialization of the log. Dispatching
		// holds the lock shared, so listeners run concurrently on the threads that log; adding and removing hold it
		// exclusively, so RemoveListener returns only once no thread runs the removed listener any more.
		class ListenerRegistry
		{
		public:
			// IDs come from an atomic counter, so one can be handed out without the lock (see Log::AddListener).
			[[nodiscard]] uint64_t CreateID()
			{
				return m_NextID.fetch_add(1, std::memory_order_relaxed);
			}

			void Add(uint64_t listenerID, LogListener listener)
			{
				std::unique_lock lock(m_Mutex);
				m_Listeners.push_back({ listenerID, std::move(listener) });
			}

			void Remove(uint64_t listenerID)
			{
				std::unique_lock lock(m_Mutex);
				std::erase_if(m_Listeners, [listenerID](const ListenerRecord& record)
				{
					return record.ID == listenerID;
				});
			}

			void Dispatch(const LogEntry& entry)
			{
				// An entry logged while this thread runs a listener (listeners must not log, but an assertion that fails
				// inside one does) is stored without being dispatched: dispatching it would recurse into the listeners and
				// take the lock a second time.
				if (s_IsDispatchingToListeners)
					return;

				std::shared_lock lock(m_Mutex);
				if (m_Listeners.empty())
					return;

				ListenerDispatchScope dispatchScope;
				for (const ListenerRecord& record : m_Listeners)
					record.Callback(entry);
			}
		private:
			std::shared_mutex m_Mutex;
			std::vector<ListenerRecord> m_Listeners; // registration order
			std::atomic<uint64_t> m_NextID{ 1 };
		};

	}

	namespace Utils {

		static ListenerRegistry& GetListenerRegistry()
		{
			static Immortal<ListenerRegistry> s_Registry;
			return s_Registry.Value;
		}

	}

	namespace {

		// Adapts spdlog's sink interface onto RingBufferSink::Append (RingBufferSink.h stays spdlog-free, Architecture §3
		// rule 3) and feeds the listeners. Each logger has its own instance, which knows the logger's channel. It takes no
		// lock of its own (null_mutex): the ring buffer is thread-safe and the listener registry has its own lock, so
		// listeners never run under a sink mutex. Its formatter is unused: the ring buffer stores structured entries.
		class RingBufferAdapterSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
		{
		public:
			// `ringBuffer` is a back-reference into the LogInstance that owns the logger holding this sink; the instance
			// destroys its loggers before its ring buffer.
			RingBufferAdapterSink(RingBufferSink& ringBuffer, LogChannel channel, std::chrono::steady_clock::time_point epoch)
				: m_RingBuffer(&ringBuffer), m_Channel(channel), m_Epoch(epoch)
			{
			}
		protected:
			void sink_it_(const spdlog::details::log_msg& message) override
			{
				const LogContext& context = LogContextScope::GetCurrent();
				const std::chrono::nanoseconds elapsed = std::chrono::steady_clock::now() - m_Epoch;

				LogEntry entry;
				entry.TimeNs = static_cast<uint64_t>(elapsed.count());
				entry.Tick = context.Tick;
				entry.Level = Utils::FromSpdlogLevel(message.level);
				entry.Logger = m_Channel;
				entry.Message.assign(message.payload.data(), message.payload.size());
				if (message.source.filename != nullptr)
					entry.File = message.source.filename;
				entry.Line = message.source.line > 0 ? static_cast<uint32_t>(message.source.line) : 0;
				entry.EntityId = context.Entity;
				entry.ScriptFile = context.ScriptFile;
				entry.ScriptLine = context.ScriptLine;

				entry.Seq = m_RingBuffer->Append(entry);
				Utils::GetListenerRegistry().Dispatch(entry);
			}

			void flush_() override
			{
			}
		private:
			RingBufferSink* m_RingBuffer = nullptr;
			LogChannel m_Channel = LogChannel::Engine;
			std::chrono::steady_clock::time_point m_Epoch;
		};

		// The rotating log file (Architecture §4.4): <stem><ext> is the newest file, then <stem>.1<ext> up to
		// <stem>.<fileCount - 1><ext>, the oldest. A file rotates when the next entry would take it past maxBytes (an entry
		// larger than maxBytes is still written whole). It uses standard streams rather than spdlog's rotating_file_sink,
		// which reports open and rename failures by throwing: first-party code catches nothing outside the boundary files
		// of Architecture §4.6, and Log::Initialize must report an Io error instead. A failure while logging cannot be
		// logged (the sink's own mutex is held), so it is absorbed: entries that cannot be written are lost to the file
		// only (the ring buffer and the console still have them); rotation truncates the file whatever happened to the
		// old one, which keeps the file within its limit even when it could not be moved; and a file that could not be
		// reopened is retried with every following entry, so file logging resumes once the file can be opened again.
		class RotatingFileSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			// `size` is the current size of the opened file. A fileCount of 0 behaves like 1: the file is truncated when full.
			RotatingFileSink(std::filesystem::path path, size_t maxBytes, size_t fileCount, std::ofstream stream, size_t size)
				: m_Path(std::move(path)), m_MaxBytes(maxBytes), m_FileCount(fileCount), m_Stream(std::move(stream)), m_CurrentSize(size)
			{
			}

			// Opens `path` for appending; its directory must exist. Errors: Io.
			[[nodiscard]] static Result<std::shared_ptr<RotatingFileSink>> Open(const std::filesystem::path& path, size_t maxBytes,
				size_t fileCount)
			{
				errno = 0;
				std::ofstream stream(path, std::ios::binary | std::ios::app);
				if (!stream.is_open())
				{
					const int errorNumber = errno;
					return MakeError(ErrorCode::Io, "cannot open the log file '{}'{}", Utils::PathToUtf8(path),
						Utils::DescribeErrno(errorNumber));
				}

				// The size only decides when the first rotation happens; an unknown size counts as empty.
				std::error_code sizeError;
				const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
				const size_t currentSize = sizeError ? 0 : static_cast<size_t>(size);
				return std::make_shared<RotatingFileSink>(path, maxBytes, fileCount, std::move(stream), currentSize);
			}
		protected:
			void sink_it_(const spdlog::details::log_msg& message) override
			{
				spdlog::memory_buf_t formatted;
				formatter_->format(message, formatted);
				if (m_CurrentSize > 0 && m_CurrentSize + formatted.size() > m_MaxBytes)
					Rotate();
				else if (!m_Stream.is_open())
					Reopen();

				if (!m_Stream.is_open())
					return;
				m_Stream.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
				m_CurrentSize += formatted.size();
			}

			void flush_() override
			{
				if (m_Stream.is_open())
					m_Stream.flush();
			}
		private:
			[[nodiscard]] std::filesystem::path GetRotatedPath(size_t index) const
			{
				if (index == 0)
					return m_Path;

				std::filesystem::path name = m_Path.stem();
				name += std::format(".{}", index);
				name += m_Path.extension();
				return m_Path.parent_path() / name;
			}

			void Rotate()
			{
				m_Stream.close();

				// <stem>.<n-2> -> <stem>.<n-1>, ..., <stem> -> <stem>.1. Renaming onto the oldest file replaces it.
				for (size_t count = m_FileCount; count > 1; --count)
				{
					std::error_code error;
					const std::filesystem::path source = GetRotatedPath(count - 2);
					if (std::filesystem::exists(source, error))
						std::filesystem::rename(source, GetRotatedPath(count - 1), error);
				}

				m_Stream.open(m_Path, std::ios::binary | std::ios::trunc);
				m_CurrentSize = 0;
			}

			// Opens the file again after a rotation could not, appending to whatever it holds by now.
			void Reopen()
			{
				m_Stream.open(m_Path, std::ios::binary | std::ios::app);
				std::error_code error;
				const std::uintmax_t size = std::filesystem::file_size(m_Path, error);
				m_CurrentSize = error ? 0 : static_cast<size_t>(size);
			}
		private:
			std::filesystem::path m_Path;
			size_t m_MaxBytes = 0;
			size_t m_FileCount = 0;
			std::ofstream m_Stream;
			size_t m_CurrentSize = 0;
		};

		// Everything one successful Log::Initialize creates. Members are destroyed in reverse order, so the loggers (and the
		// adapter sinks referring to the ring buffer) go before the ring buffer.
		struct LogInstance
		{
			Scope<RingBufferSink> RingBuffer;
			std::array<std::shared_ptr<spdlog::logger>, LogChannelCount> Loggers;
		};

		// Owns the instance of the current initialization. When a process exits without calling Log::Shutdown, this
		// destructor shuts the log down, so the published logger pointers never outlive their loggers.
		struct LogInstanceOwner
		{
			Scope<LogInstance> Instance;

			~LogInstanceOwner()
			{
				Log::Shutdown();
			}
		};

		// The fallback logger's output: formatted entries written to stderr. spdlog's own stderr sinks lock spdlog's
		// function-local console mutex, which is destroyed during static destruction; this sink owns its lock, and the C
		// stream stays open until every static destructor has run. A write that fails is lost, because no channel is left
		// to report it on; the stream's error flag is cleared so the next entry is tried again.
		class StderrSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		protected:
			void sink_it_(const spdlog::details::log_msg& message) override
			{
				spdlog::memory_buf_t formatted;
				formatter_->format(message, formatted);
				if (std::fwrite(formatted.data(), 1, formatted.size(), stderr) != formatted.size())
					std::clearerr(stderr);
			}

			void flush_() override
			{
				std::fflush(stderr);
			}
		};

		// The loggers in use before Initialize and after Shutdown: Warn and above go to stderr, the rest is discarded.
		class FallbackLog
		{
		public:
			FallbackLog()
				: m_Sink(std::make_shared<StderrSink>())
			{
				m_Sink->set_pattern(FallbackPattern);
				for (size_t index = 0; index < LogChannelCount; ++index)
				{
					const std::string name(LogChannelToString(static_cast<LogChannel>(index)));
					m_Loggers[index] = std::make_shared<spdlog::logger>(name, m_Sink);
					m_Loggers[index]->set_level(spdlog::level::warn);
					m_Loggers[index]->flush_on(spdlog::level::warn);
				}
			}

			[[nodiscard]] spdlog::logger& GetLogger(size_t index) { return *m_Loggers[index]; }
			void Flush() { m_Sink->flush(); }
		private:
			spdlog::sink_ptr m_Sink;
			std::array<std::shared_ptr<spdlog::logger>, LogChannelCount> m_Loggers;
		};

	}

	// Process-level state (Architecture §3 rule 5), replaced only by Initialize and Shutdown, which run while no other
	// thread logs (Log.h). The atomics publish the current instance to the logging threads. All of it is
	// constant-initialized, so logging works at any time, static initialization included; the lazily created fallback
	// logger, listener registry and empty ring buffer are Utils::Immortal, so it also works during static destruction.
	static std::atomic<bool> s_IsLogInitialized{ false };
	static std::array<std::atomic<spdlog::logger*>, LogChannelCount> s_ActiveLoggers{};
	static std::atomic<RingBufferSink*> s_ActiveRingBuffer{ nullptr };
	static LogInstanceOwner s_LogInstanceOwner;

	namespace Utils {

		static FallbackLog& GetFallbackLog()
		{
			static Immortal<FallbackLog> s_FallbackLog;
			return s_FallbackLog.Value;
		}

		static Result<std::shared_ptr<RotatingFileSink>> CreateFileSink(const std::filesystem::path& path)
		{
			const std::filesystem::path directory = path.parent_path();
			if (!directory.empty())
			{
				std::error_code error;
				std::filesystem::create_directories(directory, error);
				if (error)
				{
					return MakeError(ErrorCode::Io, "cannot create the log directory '{}': {}", PathToUtf8(directory),
						error.message());
				}
			}

			// LogFileCount counts every file, the current one included.
			ENGINE_TRY_ASSIGN(std::shared_ptr<RotatingFileSink> sink, RotatingFileSink::Open(path, LogFileMaxBytes, LogFileCount));
			sink->set_pattern(FilePattern);
			return sink;
		}

#if !defined(ENGINE_DIST)
		static spdlog::sink_ptr CreateConsoleSink(LogLevel minimumLevel)
		{
			// stderr keeps stdout free for machine-readable output, and death-test parents read assert messages there.
			spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
			sink->set_pattern(ConsolePattern);
			sink->set_level(Detail::ToSpdlogLevel(minimumLevel));
			return sink;
		}
#endif

	}

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

	Status Log::Initialize(const LogSpecification& specification)
	{
		ENGINE_CORE_ASSERT(!IsInitialized(), "Log::Initialize was called while the log is initialized");
		if (IsInitialized())
			return MakeError(ErrorCode::InvalidState, "the log is already initialized");

		// Every fallible step comes first, so a failure replaces nothing.
		std::vector<spdlog::sink_ptr> outputSinks;
		if (!specification.FilePath.empty())
		{
			ENGINE_TRY_ASSIGN(spdlog::sink_ptr fileSink, Utils::CreateFileSink(specification.FilePath));
			outputSinks.push_back(std::move(fileSink));
		}
#if !defined(ENGINE_DIST)
		if (specification.Console)
			outputSinks.push_back(Utils::CreateConsoleSink(specification.ConsoleLevel));
#endif

		Scope<LogInstance> instance = CreateScope<LogInstance>();
		instance->RingBuffer = CreateScope<RingBufferSink>(specification.RingBufferCapacity);
		const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
		for (size_t index = 0; index < LogChannelCount; ++index)
		{
			const LogChannel channel = static_cast<LogChannel>(index);

			// The ring buffer comes first, so an entry is stored (for crash reports) before any output sink runs.
			std::vector<spdlog::sink_ptr> sinks;
			sinks.reserve(outputSinks.size() + 1);
			sinks.push_back(std::make_shared<RingBufferAdapterSink>(*instance->RingBuffer, channel, epoch));
			sinks.insert(sinks.end(), outputSinks.begin(), outputSinks.end());

			auto logger = std::make_shared<spdlog::logger>(std::string(LogChannelToString(channel)), sinks.begin(), sinks.end());
			// Trace is compiled out of the macros in Dist; the level keeps direct Script logger calls consistent with that.
			logger->set_level(Detail::LogTraceEnabled ? spdlog::level::trace : spdlog::level::info);
			// Warnings and errors reach the file at once, even if the process later dies without flushing.
			logger->flush_on(spdlog::level::warn);
			instance->Loggers[index] = std::move(logger);
		}

		for (size_t index = 0; index < LogChannelCount; ++index)
			s_ActiveLoggers[index].store(instance->Loggers[index].get(), std::memory_order_release);
		s_ActiveRingBuffer.store(instance->RingBuffer.get(), std::memory_order_release);
		s_LogInstanceOwner.Instance = std::move(instance);
		s_IsLogInitialized.store(true, std::memory_order_release);
		return {};
	}

	void Log::Shutdown()
	{
		if (!s_IsLogInitialized.exchange(false, std::memory_order_acq_rel))
			return;

		for (std::atomic<spdlog::logger*>& logger : s_ActiveLoggers)
			logger.store(nullptr, std::memory_order_release);
		s_ActiveRingBuffer.store(nullptr, std::memory_order_release);

		Scope<LogInstance> instance = std::move(s_LogInstanceOwner.Instance);
		for (const std::shared_ptr<spdlog::logger>& logger : instance->Loggers)
			logger->flush();
		instance.reset();
	}

	bool Log::IsInitialized()
	{
		return s_IsLogInitialized.load(std::memory_order_acquire);
	}

	void Log::Flush()
	{
		bool flushedActiveLogger = false;
		for (std::atomic<spdlog::logger*>& activeLogger : s_ActiveLoggers)
		{
			if (spdlog::logger* logger = activeLogger.load(std::memory_order_acquire))
			{
				logger->flush();
				flushedActiveLogger = true;
			}
		}
		if (!flushedActiveLogger)
			Utils::GetFallbackLog().Flush();
	}

	spdlog::logger& Log::GetLogger(LogChannel channel)
	{
		size_t index = std::to_underlying(channel);
		if (index >= LogChannelCount)
		{
			ENGINE_CORE_ASSERT(false, "Unknown LogChannel {}", index);
			index = std::to_underlying(LogChannel::Engine);
		}

		if (spdlog::logger* logger = s_ActiveLoggers[index].load(std::memory_order_acquire))
			return *logger;
		return Utils::GetFallbackLog().GetLogger(index);
	}

	RingBufferSink& Log::GetRingBuffer()
	{
		if (RingBufferSink* ringBuffer = s_ActiveRingBuffer.load(std::memory_order_acquire))
			return *ringBuffer;

		static Utils::Immortal<RingBufferSink> s_EmptyRingBuffer{ size_t{ 1 } };
		return s_EmptyRingBuffer.Value;
	}

	uint64_t Log::AddListener(LogListener listener)
	{
		ENGINE_CORE_ASSERT(static_cast<bool>(listener), "Log::AddListener needs a callable listener");
		// Registering from inside a listener would wait for the dispatch this thread is running.
		ENGINE_CORE_ASSERT(!s_IsDispatchingToListeners, "Log::AddListener must not be called from a log listener");

		// Where the asserts are compiled out (Dist), such a listener gets an ID but is never registered: calling an empty
		// std::function throws, and registering from a listener would deadlock.
		ListenerRegistry& registry = Utils::GetListenerRegistry();
		const uint64_t listenerID = registry.CreateID();
		if (listener && !s_IsDispatchingToListeners)
			registry.Add(listenerID, std::move(listener));
		return listenerID;
	}

	void Log::RemoveListener(uint64_t listenerID)
	{
		// A listener cannot remove itself (or another one) while running: the removal would wait for this very dispatch.
		ENGINE_CORE_ASSERT(!s_IsDispatchingToListeners, "Log::RemoveListener must not be called from a log listener");
		if (s_IsDispatchingToListeners)
			return;
		Utils::GetListenerRegistry().Remove(listenerID);
	}

}
