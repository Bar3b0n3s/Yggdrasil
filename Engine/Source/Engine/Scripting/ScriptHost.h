#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scripting/ScriptError.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	class AssetManager;
	class AudioSystem;
	class DebugDrawList;
	class IFieldSchemaSource;
	class PhysicsSystem;
	class Random;
	class Scene;
	class TypeRegistry;

	struct ScriptFrameState
	{
		InputPhase Phase = InputPhase::Step;
		uint64_t Tick = 0;
		uint64_t Frame = 0;
		double DeltaTime = 0.0;
		double FixedDeltaTime = 1.0 / 60.0;
		double TimeScale = 1.0;
		float InterpolationAlpha = 1.0f;
	};

	struct ScriptEnvironment
	{
		bool IsEditor = false;
		bool IsHeadless = true;
		bool IsFocused = false;
		std::string Platform{};
		std::string Version{};
		glm::vec2 WindowSize{ 0.0f };
	};

	struct ScriptActionState
	{
		bool Down = false;
		bool Pressed = false;
		bool Released = false;
		float Axis = 0.0f;
	};

	// The only upward-facing boundary of Scripting (§3). A Session adapter supplies these lower-layer services; the VM
	// never includes Session, Testing, App or editor headers. Main-thread-only borrowed back-reference, outliving its VM.
	// Methods receiving external values validate them and return located errors; they never assert on a script value.
	class IScriptHost
	{
	public:
		virtual ~IScriptHost() = default;

		[[nodiscard]] virtual Scene& GetScene() = 0;
		[[nodiscard]] virtual const TypeRegistry& GetTypes() const = 0;
		[[nodiscard]] virtual AssetManager* GetAssets() = 0;
		[[nodiscard]] virtual const IFieldSchemaSource* GetFieldSchemas() const = 0;
		[[nodiscard]] virtual PhysicsSystem* GetPhysics() = 0;
		[[nodiscard]] virtual AudioSystem* GetAudio() = 0;
		[[nodiscard]] virtual DebugDrawList* GetDebugDraw() = 0;
		[[nodiscard]] virtual Random& GetRandom() = 0;
		[[nodiscard]] virtual const InputState& GetInput() const = 0;
		[[nodiscard]] virtual ScriptFrameState GetFrameState() const = 0;
		[[nodiscard]] virtual ScriptEnvironment GetEnvironment() const = 0;
		// Nonzero identity of this scene incarnation; never reused while a proxy from an older VM might exist.
		[[nodiscard]] virtual uint64_t GetSceneGeneration() const = 0;
		[[nodiscard]] virtual const Json& GetLoadParameters() const = 0;
		// Combines physical and injected actions in the current phase. Unknown names return INPUT_UNKNOWN_ACTION with hints.
		[[nodiscard]] virtual Result<ScriptActionState> GetAction(std::string_view name) const = 0;

		// Immediate scene mutation, seeded IDs and entity-cap checks. The binding creates script instances and calls
		// OnCreate before returning. A null parent means root. Instantiation uses the shared PrefabInstantiator path.
		[[nodiscard]] virtual Result<UUID> CreateEntity(std::string_view name, UUID parent) = 0;
		[[nodiscard]] virtual Result<UUID> Instantiate(AssetHandle prefab, const std::optional<glm::vec3>& position,
			const std::optional<glm::quat>& rotation, UUID parent) = 0;
		virtual void MarkTeleported(UUID entity) = 0;
		[[nodiscard]] virtual Status SetTimeScale(double scale) = 0;
		[[nodiscard]] virtual Status SetCursorMode(CursorMode mode) = 0;
		[[nodiscard]] virtual CursorMode GetCursorMode() const = 0;

		// Requests never destroy their own running VM. Scene.Load is consumed at the end of the frame, after protected
		// calls unwind; it starts a fresh VM with the parameters and preserves the session's clock and replay owner.
		[[nodiscard]] virtual Status RequestSceneLoad(AssetHandle scene, const Json& parameters) = 0;
		virtual void RequestQuit(int32_t exitCode) = 0;
		virtual void RequestPause() = 0;
		// Publish to Script logging, EventLog and host observability. fatal means second/hard memory breach; stop safely.
		virtual void OnScriptError(const ScriptError& error, bool fatal) = 0;
		// Called immediately before an eval/test driver mutates gameplay state or the shared random stream, including
		// on a call that later fails. Invalidates input-only recording; normal gameplay callbacks do not call this.
		// Test input injection is recorded input, so it is not an external state mutation.
		virtual void OnExternalMutation(std::string_view reason) = 0;
		// True in lockstep, recording, replay or test runs. Explicit Test.ReloadScript uses the tested synchronous override.
		[[nodiscard]] virtual bool IsReloadDeferred() const = 0;
	};

}
