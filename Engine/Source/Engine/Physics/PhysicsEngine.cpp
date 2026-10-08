#include "EnginePCH.h"
#include "Engine/Physics/PhysicsEngine.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/Private/PhysicsEngineState.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <source_location>
#include <thread>

namespace Engine {

	// Jolt's process-level state (Architecture §3 rule 5, §9.1): ProcessContext initializes it once, on the process's main
	// thread, which is the only thread that changes it. IsInitialized and the live counters may be read from any thread.
	static std::atomic<bool> s_IsInitialized{ false };
	static std::thread::id s_MainThread;
	static Scope<JPH::Factory> s_Factory;
	static Scope<JPH::JobSystemThreadPool> s_JobSystem;
	static uint32_t s_WorkerThreadCount = 0;
	// The hooks Initialize replaced, which Shutdown puts back.
	static JPH::TraceFunction s_PreviousTrace = nullptr;
#ifdef JPH_ENABLE_ASSERTS
	static JPH::AssertFailedFunction s_PreviousAssertFailed = nullptr;
#endif
	// The Jolt systems of the live PhysicsWorld objects (non-owning: each world unregisters its system before destroying it),
	// guarded by s_WorldsMutex.
	static std::mutex s_WorldsMutex;
	static std::vector<const JPH::PhysicsSystem*> s_Worlds;

	namespace Utils {

		// Architecture §4.11: clamp(hardware concurrency - 2, 1, 4); an unknown hardware concurrency counts as 0.
		[[nodiscard]] static uint32_t GetDefaultWorkerThreadCount()
		{
			const int64_t available = static_cast<int64_t>(std::thread::hardware_concurrency()) - 2;
			return static_cast<uint32_t>(std::clamp<int64_t>(available, PhysicsEngineSpecification::MinDefaultWorkerThreads,
				PhysicsEngineSpecification::MaxDefaultWorkerThreads));
		}

		// JPH::Trace: Jolt's diagnostic lines, on any thread (worker threads during a step included), to the Engine logger.
		static void OnJoltTrace(const char* format, ...)
		{
			if (format == nullptr)
				return;

			std::va_list arguments;
			va_start(arguments, format);
			std::va_list measuring;
			va_copy(measuring, arguments);
			const int length = std::vsnprintf(nullptr, 0, format, measuring);
			va_end(measuring);
			std::string text;
			if (length > 0)
			{
				text.resize(static_cast<size_t>(length));
				// Writes the measured characters and the terminating null, which the string already holds.
				static_cast<void>(std::vsnprintf(text.data(), text.size() + 1, format, arguments));
			}
			va_end(arguments);

			if (length < 0)
				text = format; // a format vsnprintf cannot expand: its text as it is
			ENGINE_CORE_INFO("Jolt: {}", text);
		}

#ifdef JPH_ENABLE_ASSERTS
		// The one Jolt assert content can reach: PhysicsSystem::Update asserts whenever a step overflowed one of its caches
		// (too many body pairs or contacts), and returns the same condition as an EPhysicsUpdateError, which PhysicsWorld::Step
		// reports as PHYSICS_LIMIT_EXCEEDED while the session keeps running (§9.1, ADR 0014 decision 11). Its expression, as
		// JPH_ASSERT spells it (Jolt/Physics/PhysicsSystem.cpp).
		constexpr std::string_view JoltUpdateErrorAssertExpression = "errors == EPhysicsUpdateError::None";

		// JPH::AssertFailed: a failed Jolt assert is a programmer error (content never reaches one, §9.1), reported to the
		// engine's assert handler like ENGINE_CORE_ASSERT, which ends the process. The message names Jolt's file and line.
		static bool OnJoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			if (expression != nullptr && std::string_view(expression) == JoltUpdateErrorAssertExpression)
				return false; // no breakpoint: the step's error is returned and reported
			Detail::ReportAssertFailure(AssertKind::Assert, false, expression != nullptr ? expression : "",
				std::format("Jolt: {} ({}:{})", message != nullptr ? message : "(no message)", file != nullptr ? file : "(unknown file)", line),
				std::source_location::current());
			// The handler never returns (Assert.h); no breakpoint is requested if one did.
			return false;
		}
#endif

	}

	Status PhysicsEngine::Initialize(const PhysicsEngineSpecification& specification)
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized.load(), "The physics engine is already initialized: PhysicsEngine::Initialize called twice without Shutdown");
		if (s_IsInitialized.load())
			return MakeError(ErrorCode::InvalidState, "the physics engine is already initialized");

		const uint32_t workerThreads = specification.WorkerThreads.value_or(Utils::GetDefaultWorkerThreadCount());
		if (workerThreads > PhysicsEngineSpecification::MaxWorkerThreads)
		{
			return MakeError(ErrorCode::InvalidArgument, "the physics engine takes 0 to {} worker threads (got {})", PhysicsEngineSpecification::MaxWorkerThreads,
				workerThreads);
		}

		// §9.1, in order.
		JPH::RegisterDefaultAllocator();
		s_PreviousTrace = JPH::Trace;
		JPH::Trace = &Utils::OnJoltTrace;
#ifdef JPH_ENABLE_ASSERTS
		s_PreviousAssertFailed = JPH::AssertFailed;
		JPH::AssertFailed = &Utils::OnJoltAssertFailed;
#endif
		// A consumer compiled with other Jolt defines than the library would corrupt memory (Vendor/JoltPhysics/VENDOR.md).
		if (!JPH::VerifyJoltVersionID())
			FatalError(FatalErrorKind::InitFailed, "Jolt Physics was built with other defines than the engine (JPH::VerifyJoltVersionID failed)");

		s_Factory = CreateScope<JPH::Factory>();
		JPH::Factory::sInstance = s_Factory.get();
		JPH::RegisterTypes();
		s_JobSystem = CreateScope<JPH::JobSystemThreadPool>(MaxPhysicsJobs, MaxPhysicsBarriers, static_cast<int>(workerThreads));
		s_WorkerThreadCount = workerThreads;
		s_MainThread = std::this_thread::get_id();
		s_IsInitialized.store(true);
		ENGINE_CORE_INFO("Physics engine initialized: Jolt {}.{}.{}, {} worker thread(s)", JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH,
			workerThreads);
		return {};
	}

	void PhysicsEngine::Shutdown()
	{
		if (!s_IsInitialized.load())
			return;

		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsEngine::Shutdown called off the main thread");
		// A world that outlived the engine would later free its bodies through an unregistered factory and a dead job system.
		ENGINE_CORE_VERIFY(GetLiveWorldCount() == 0 && GetLiveBodyCount() == 0,
			"PhysicsEngine::Shutdown with {} live PhysicsWorld(s) and {} live bodies: destroy every PhysicsWorld first", GetLiveWorldCount(),
			GetLiveBodyCount());

		s_JobSystem.reset();
		JPH::UnregisterTypes();
		JPH::Factory::sInstance = nullptr;
		s_Factory.reset();
#ifdef JPH_ENABLE_ASSERTS
		JPH::AssertFailed = s_PreviousAssertFailed;
		s_PreviousAssertFailed = nullptr;
#endif
		JPH::Trace = s_PreviousTrace;
		s_PreviousTrace = nullptr;
		s_WorkerThreadCount = 0;
		s_IsInitialized.store(false);
		s_MainThread = std::thread::id();
	}

	bool PhysicsEngine::IsInitialized()
	{
		return s_IsInitialized.load();
	}

	uint32_t PhysicsEngine::GetWorkerThreadCount()
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized.load() || Detail::IsPhysicsMainThread(), "PhysicsEngine::GetWorkerThreadCount called off the main thread");
		return s_WorkerThreadCount;
	}

	Status PhysicsEngine::SetWorkerThreadCount(uint32_t count)
	{
		if (!s_IsInitialized.load())
			return MakeError(ErrorCode::InvalidState, "the physics engine is not initialized");
		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsEngine::SetWorkerThreadCount called off the main thread");
		if (count > PhysicsEngineSpecification::MaxWorkerThreads)
			return MakeError(ErrorCode::InvalidArgument, "the physics engine takes 0 to {} worker threads (got {})", PhysicsEngineSpecification::MaxWorkerThreads, count);

		// Between steps (main thread only, and PhysicsWorld::Step returns when its jobs are done), so no job is running.
		s_JobSystem->SetNumThreads(static_cast<int>(count));
		s_WorkerThreadCount = count;
		return {};
	}

	uint32_t PhysicsEngine::GetLiveWorldCount()
	{
		const std::scoped_lock lock(s_WorldsMutex);
		return static_cast<uint32_t>(s_Worlds.size());
	}

	uint32_t PhysicsEngine::GetLiveBodyCount()
	{
		const std::scoped_lock lock(s_WorldsMutex);
		uint32_t bodies = 0;
		for (const JPH::PhysicsSystem* system : s_Worlds)
			bodies += system->GetNumBodies();
		return bodies;
	}

	namespace Detail {

		JPH::JobSystem* GetPhysicsJobSystem()
		{
			return s_JobSystem.get();
		}

		bool IsPhysicsMainThread()
		{
			return s_IsInitialized.load() && std::this_thread::get_id() == s_MainThread;
		}

		void RegisterPhysicsWorld(const JPH::PhysicsSystem& system)
		{
			const std::scoped_lock lock(s_WorldsMutex);
			ENGINE_CORE_ASSERT(std::find(s_Worlds.begin(), s_Worlds.end(), &system) == s_Worlds.end(), "A physics world was registered twice");
			s_Worlds.push_back(&system);
		}

		void UnregisterPhysicsWorld(const JPH::PhysicsSystem& system)
		{
			const std::scoped_lock lock(s_WorldsMutex);
			const auto found = std::find(s_Worlds.begin(), s_Worlds.end(), &system);
			ENGINE_CORE_ASSERT(found != s_Worlds.end(), "A physics world was unregistered without being registered");
			if (found != s_Worlds.end())
				s_Worlds.erase(found);
		}

	}

}
