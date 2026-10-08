#include "TestsPCH.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Scene/Entity.h"
#include "Support/ProtocolTestTypes.h"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <utility>

// The shared host context of the Editor and the Runtime (ADR 0008 decisions 7 and 26, Docs/Decisions/0012-m7-decisions.md
// decision 12). Its IsHostType is implemented by the contract.

namespace Engine {

	namespace {

		// A host context of the shared base with every service refused, enough to check the host-type rules.
		class StubHostContext final : public AutomationMethodContext
		{
		public:
			explicit StubHostContext(MethodRequest request)
				: AutomationMethodContext(TypeKeyOf<StubHostContext>(), std::move(request))
			{
			}

			[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override
			{
				return CreateScope<StubHostContext>(std::move(request));
			}

			[[nodiscard]] Result<Scene*> ResolveTargetScene(SceneTarget /*target*/, bool /*given*/, bool /*mutation*/) const override
			{
				return MakeError(ErrorCode::InvalidState, "no scene");
			}

			[[nodiscard]] Result<Entity> ResolveEntity(Scene& /*scene*/, std::string_view /*reference*/, std::string_view /*pointer*/) const override
			{
				return MakeError(ErrorCode::NotFound, "no entity");
			}

			[[nodiscard]] EntitySummary MakeEntitySummary(ConstEntity /*entity*/) const override { return {}; }
			[[nodiscard]] PlaySession* GetPlaySession() const override { return nullptr; }
			[[nodiscard]] Status StartPlay(const PlayStartOptions& /*options*/) override { return MakeError(ErrorCode::Unsupported, "no play"); }
			[[nodiscard]] Status StopPlay() override { return MakeError(ErrorCode::Unsupported, "no play"); }
			[[nodiscard]] std::string GetClientName(ClientId /*client*/) const override { return {}; }
			[[nodiscard]] EventLog& GetEventLog() const override { return m_Events; }

			[[nodiscard]] Result<Image> CaptureView(const RenderSnapshot& /*snapshot*/, const ViewportScreenshotRequest& /*request*/) override
			{
				return MakeError(ErrorCode::Unsupported, "no renderer");
			}

			[[nodiscard]] std::optional<ExplicitRenderCamera> GetSceneViewCamera() const override { return std::nullopt; }

			[[nodiscard]] Result<std::string> WriteOutputFile(std::string_view /*extension*/, std::span<const std::byte> /*bytes*/) override
			{
				return MakeError(ErrorCode::Unsupported, "no output");
			}

			[[nodiscard]] SessionHostDescription DescribeSession() const override { return SessionHostDescription{ .Renderer = "none" }; }

			[[nodiscard]] Result<SessionShutdownResult> Shutdown(const SessionShutdownParams& /*params*/) override
			{
				return MakeError(ErrorCode::Unsupported, "no shutdown");
			}

			[[nodiscard]] SceneSummary MakeSceneSummary(const Scene& /*scene*/) const override { return {}; }

			[[nodiscard]] AssetManager* GetAssets() const override { return nullptr; }

			[[nodiscard]] std::chrono::steady_clock::time_point GetWallClockTime() const override { return std::chrono::steady_clock::time_point(); }
		private:
			mutable EventLog m_Events;
		};

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("AutomationMethodContext: a host context answers for itself, the shared base and MethodContext")
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			const MethodRegistry methods(*types);
			MethodDescriptor descriptor;
			descriptor.Specification.Name = "play.state";
			descriptor.Domain = "play";
			const StubHostContext context(MethodRequest{
				.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "play.state", .TranscriptLine = std::nullopt },
				.Options = {},
				.Method = &descriptor,
				.Params = Json::object(),
				.Registry = &methods,
				.PhaseMarker = nullptr,
				.NestingDepth = 0,
			});
			CHECK(context.GetHostKey() == TypeKeyOf<StubHostContext>());
			CHECK(context.IsHostType(TypeKeyOf<StubHostContext>()));
			CHECK(context.IsHostType(TypeKeyOf<AutomationMethodContext>()));
			CHECK(context.IsHostType(TypeKeyOf<MethodContext>()));
			CHECK_FALSE(context.IsHostType(TypeKeyOf<Test::TestHostContext>()));
		}
	}

}
