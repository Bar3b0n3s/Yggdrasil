#include "EditorPCH.h"
#include "EditorCore/Automation/DebugMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace Engine {

	namespace {

		// debug.stall's bound (DebugStallParams::Ms).
		constexpr uint32_t MaxStallMilliseconds = 60000;

		// debug.pend's operation: resolves after its frames (one Poll per frame), or never for 0, until cancelled.
		class DebugPendOperation final : public PendingOperation
		{
		public:
			DebugPendOperation(uint32_t frames, std::string clientName)
				: m_Frames(frames), m_ClientName(std::move(clientName))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				++m_Polls;
				if (m_Frames == 0 || m_Polls < m_Frames)
					return std::nullopt;
				DebugPendResult result;
				result.Frames = m_Polls;
				return context.SerializeResult(result);
			}

			void Cancel(MethodContext& /*context*/) override
			{
				ENGINE_INFO("Cancelled debug.pend of client '{}'", m_ClientName);
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				return m_Frames == 0 ? std::format("Debug:pend {}", m_Polls) : std::format("Debug:pend {}/{}", m_Polls, m_Frames);
			}
		private:
			uint32_t m_Frames = 0;
			uint32_t m_Polls = 0;
			std::string m_ClientName;
		};

	}

	namespace Automation {

		Result<DebugStallResult> DebugStall(EditorMethodContext& context, const DebugStallParams& params)
		{
			if (params.Ms > MaxStallMilliseconds)
			{
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/ms", std::format("ms must be at most {}", MaxStallMilliseconds)));
			}
			// The test hook freezes the main thread on purpose, so the watchdog answers other clients with Busy and this phase.
			context.SetPhase(std::format("Debug:stall {} ms", params.Ms));
			std::this_thread::sleep_for(std::chrono::milliseconds(params.Ms));
			DebugStallResult result;
			result.StalledMs = params.Ms;
			return result;
		}

		Result<Scope<PendingOperation>> DebugPend(EditorMethodContext& context, const DebugPendParams& params)
		{
			const std::string& client = context.GetRequest().ClientName;
			ENGINE_INFO("Started debug.pend of client '{}'", client);
			return Scope<PendingOperation>(CreateScope<DebugPendOperation>(params.Frames, client));
		}

		Result<DebugDeviceLostResult> DebugDeviceLost(EditorMethodContext& context, const NoParams& /*params*/)
		{
			const auto& specification = context.GetServer().GetSpecification();
			if (specification.RendererName == "none" || !specification.QueueDeviceLost)
				return MakeError(ErrorCode::Unsupported, "This host cannot queue an injected device loss");
			ENGINE_TRY(specification.QueueDeviceLost());
			return DebugDeviceLostResult{ .Queued = true };
		}

	}

	void RegisterDebugMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<DebugStallParams>("DebugStallParams", "The params of the test hook debug.stall.")
			.Field("ms", &DebugStallParams::Ms, "How long to block the main thread, in milliseconds.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxStallMilliseconds), .Unit = "ms" });

		registry.Struct<DebugStallResult>("DebugStallResult", "What the test hook debug.stall did.")
			.Field("stalledMs", &DebugStallResult::StalledMs, "How long the main thread was blocked, in milliseconds.", { .Unit = "ms" });

		registry.Struct<DebugPendParams>("DebugPendParams", "The params of the test hook debug.pend.")
			.Field("frames", &DebugPendParams::Frames, "After how many frames the operation resolves; 0: never, until its client disconnects.");

		registry.Struct<DebugPendResult>("DebugPendResult", "How the test hook debug.pend resolved.")
			.Field("frames", &DebugPendResult::Frames, "The frames (polls) it took.");

		registry.Struct<DebugDeviceLostResult>("DebugDeviceLostResult", "Whether the rendering host queued an injected device loss.")
			.Field("queued", &DebugDeviceLostResult::Queued, "The fault is queued for a submission after the host publishes its autosave snapshot.");
	}

	void RegisterDebugMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "debug.deviceLost",
						.Description = "Test hook: queue device loss on the next rendering submission after publishing the editor's autosave snapshot.",
						.TestHook = true,
						.Examples = { { .Description = "Exercise crash recovery after making a dirty edit.", .Params = Json::object() } } },
			&Automation::DebugDeviceLost);

		Json stallExample = Json::object();
		stallExample["ms"] = 100;
		methods.Add(
			{
				.Name = "debug.stall",
				.Description = "Test hook: blocks the main thread for ms milliseconds with the phase \"Debug:stall <ms> ms\", so the watchdog "
							   "answers other requests with Busy.",
				.RequiredParams = { "ms" },
				.TestHook = true,
				.TimeoutSeconds = 120,
				.Examples = { { .Description = "Freeze the main thread for 100 ms.", .Params = stallExample } },
			},
			&Automation::DebugStall);

		Json pendExample = Json::object();
		pendExample["frames"] = 3;
		methods.AddPending<EditorMethodContext, DebugPendParams, DebugPendResult>(
			{
				.Name = "debug.pend",
				.Description = "Test hook: a pending operation that resolves after frames frames, or never for 0, until its client "
							   "disconnects (which cancels it).",
				.TestHook = true,
				.TimeoutSeconds = 3600,
				.Examples = { { .Description = "Resolve after three frames.", .Params = pendExample } },
			},
			&Automation::DebugPend);
	}

}
