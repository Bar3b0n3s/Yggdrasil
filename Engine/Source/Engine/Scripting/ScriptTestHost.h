#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptReference.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	struct ScriptTestCase
	{
		std::string Name{};
		ScriptReference Function{};
		uint32_t TimeoutTicks = 0; // zero inherits the runner's effective case timeout
		std::string File{};
		uint32_t Line = 0;
	};

	struct ScriptCollectedSuite
	{
		std::string Name{};
		std::string File{};
		uint32_t Line = 0;
		std::vector<ScriptTestCase> Cases{}; // declaration order, including cases a runner later filters out
	};

	enum class ScriptCaseState : uint8_t
	{
		Yielded,
		Completed,
		Failed,
		Skipped
	};

	struct ScriptCaseResume
	{
		ScriptCaseState State = ScriptCaseState::Yielded;
		std::string Message{};
		std::string File{}; // terminal site or most recent wait site for tick-timeout attribution
		uint32_t Line = 0;
		// Failed with Error is a script fault, Failed without Error is Test.Fail. Completion does not erase failed
		// expectations reported earlier to the host. All strings and traceback frames are independent of VM lifetime.
		std::optional<ScriptError> Error{};
	};

	enum class ScriptTestSignal : uint8_t
	{
		ExpectationFailed,
		Fail,
		Skip
	};

	struct ScriptTestReport
	{
		ScriptTestSignal Signal = ScriptTestSignal::ExpectationFailed;
		std::string Message{};
		std::string File{};
		uint32_t Line = 0;
	};

	struct ScriptExtractionState
	{
		float Alpha = 1.0f;
		uint64_t Frame = 0;
	};

	// A Testing-owned main-thread back-reference. The engine specification may install it before setup callbacks;
	// otherwise CollectSuite installs it until EndSuite. It outlives every retained case/task and any setup/teardown
	// callback using it; EndSuite cancels suite work and captures before restoring the original host. Scripting never
	// includes Testing or Session. Expected errors are values, not assertions on external test-script input.
	class IScriptTestHost
	{
	public:
		virtual ~IScriptTestHost() = default;

		virtual void Report(const ScriptTestReport& report) = 0;
		// Every script fault, including setup/teardown and replacement-VM faults, is forwarded exactly once before
		// its VM can be destroyed. The runner owns the copy; it must not reconstruct history from the current VM.
		virtual void OnScriptError(const ScriptError& error, bool fatal) = 0;
		// Claims one already-reported, nonfatal occurrence for the active case after ExpectScriptError matches it.
		// Called at most once for that occurrence, in the same VM/case that observed it. It remains in the error stream
		// and suite's ScriptErrors count; only its contribution to the case's failures is removed. Setup/teardown errors,
		// fatal stops and a fault of the case body cannot be claimed. The runner defers unmatched-error classification
		// until the case ends so the script can register an expectation after the fault was observed in that case.
		virtual void OnExpectedScriptError(const ScriptError& error) = 0;
		// One camelCase input event, without tick, validated by Session's common input codec and queued for the next
		// unapplied tick. Recording observes application, never this call. Invalid input queues nothing.
		[[nodiscard]] virtual Status InjectInput(const Json& event) = 0;
		[[nodiscard]] virtual Status CaptureScreenshot(std::string_view name) = 0;
		// CaptureAudio checks yieldability first. Capture exactly the sample interval of ticks fixed steps starting
		// with the current step. A multi-step frame may pull those samples after the logical wait expires: readiness
		// then delays resumption until the next phase 4, without including earlier/later samples in the measurement.
		// No extra audio pull. Cancellation/fault/case end always cancels unfinished capture. ticks must be positive
		// and fit AudioEngine::MaxCaptureFrames at the session's fixed rate; otherwise InvalidArgument before capture.
		[[nodiscard]] virtual Status BeginAudioCapture(uint32_t ticks) = 0;
		[[nodiscard]] virtual bool IsAudioCaptureReady() const = 0;
		[[nodiscard]] virtual Result<AudioLevels> EndAudioCapture() = 0;
		virtual void CancelAudioCapture() = 0;
		// The synchronous EditorOnly exception to normal deterministic-session reload deferral.
		[[nodiscard]] virtual Status ReloadScript(AssetHandle script) = 0;
		[[nodiscard]] virtual uint64_t ComputeStateHash() const = 0;
		[[nodiscard]] virtual ScriptExtractionState GetLastExtraction() const = 0;
		[[nodiscard]] virtual std::optional<glm::vec3> GetExtractedPosition(UUID entity) const = 0;
		virtual void OnDebugBreak() = 0;
	};

}
