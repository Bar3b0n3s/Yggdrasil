#include "TestsPCH.h"
#include "Support/ProtocolTestTypes.h"

#include "Engine/Scene/Components/BuiltinComponents.h"

#include <thread>

namespace Engine {

	namespace Test {

		namespace {

			// test.read's params (none) and result.
			struct ReadParams
			{
			};

			struct ReadResult
			{
				uint32_t Revision = 0;
			};

			class PendOperation final : public PendingOperation
			{
			public:
				explicit PendOperation(uint32_t polls)
					: m_Target(polls)
				{
				}

				std::optional<Result<Json>> Poll(MethodContext& context) override
				{
					++m_Polls;
					if (m_Polls < m_Target)
						return std::nullopt;
					return context.SerializeResult(PendResult{ .Polls = m_Polls });
				}

				void Cancel(MethodContext& context) override
				{
					static_cast<TestHostContext&>(context).GetState().Calls.push_back("cancel test.pend");
				}
			private:
				uint32_t m_Target = 1;
				uint32_t m_Polls = 0;
			};

		}

		static Result<EchoResult> Echo(TestHostContext& context, const EchoParams& params)
		{
			context.GetState().Calls.push_back("handler test.echo");
			EchoResult result{ .Text = params.Text, .Count = params.Count, .Shape = params.Shape, .ComponentNames = {} };
			for (const auto& [name, value] : params.Components)
				result.ComponentNames.push_back(name);
			return result;
		}

		static Result<ReadResult> Read(TestHostContext& context, const ReadParams& /*params*/)
		{
			context.GetState().Calls.push_back("handler test.read");
			return ReadResult{ .Revision = context.GetState().Revision };
		}

		static Result<LargeResult> Large(TestHostContext& /*context*/, const LargeParams& params)
		{
			LargeResult result;
			for (uint32_t index = 0; index < params.Count; ++index)
				result.Items.push_back(std::string(64, static_cast<char>('a' + index % 26)));
			return result;
		}

		static Result<Scope<PendingOperation>> Pend(TestHostContext& /*context*/, const PendParams& params)
		{
			return Scope<PendingOperation>(CreateScope<PendOperation>(params.Polls));
		}

		static Result<ReadResult> Fail(TestHostContext& /*context*/, const ReadParams& /*params*/)
		{
			return MakeError(ErrorCode::NotFound, "nothing here");
		}

		static Result<ReadResult> Throw(TestHostContext& /*context*/, const ReadParams& /*params*/)
		{
			// std::out_of_range from the standard library, which the Dispatcher's boundary must contain.
			const std::vector<uint32_t> empty;
			return ReadResult{ .Revision = empty.at(1) };
		}

		static Result<ReadResult> ThrowSystemError(TestHostContext& /*context*/, const ReadParams& /*params*/)
		{
			// std::system_error from the standard library (joining a thread that is not joinable): the base of the
			// vk::SystemError that vulkan.hpp throws out of NVRHI, which the Dispatcher hands to the host's
			// SystemErrorHandler first.
			std::thread idle;
			idle.join();
			return ReadResult{};
		}

		TestHostContext::TestHostContext(TestHostState& state, MethodRequest request)
			: MethodContext(TypeKeyOf<TestHostContext>(), std::move(request)), m_State(&state)
		{
		}

		Scope<MethodContext> TestHostContext::CreateNested(MethodRequest request) const
		{
			return CreateScope<TestHostContext>(*m_State, std::move(request));
		}

		TestMethodHost::TestMethodHost(TestHostState& state)
			: m_State(&state)
		{
		}

		Status TestMethodHost::CheckAvailability(const MethodDescriptor& method) const
		{
			m_State->Calls.push_back("available " + method.Specification.Name);
			if (m_State->LauncherState && !method.Specification.AvailableInLauncher)
				return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
			return {};
		}

		Scope<MethodContext> TestMethodHost::CreateContext(MethodRequest request)
		{
			return CreateScope<TestHostContext>(*m_State, std::move(request));
		}

		Status TestMethodHost::AdmitRequest(MethodContext& context)
		{
			m_State->Calls.push_back("admit " + context.GetRequest().Method);
			if (m_State->DenyEverything)
				return MakeError(ErrorCode::PermissionDenied, "the test host denies '{}'", context.GetRequest().Method);
			return {};
		}

		void TestMethodHost::EnterInvocation(MethodContext& context)
		{
			m_State->Calls.push_back("enter " + context.GetRequest().Method);
		}

		void TestMethodHost::LeaveInvocation(MethodContext& context)
		{
			m_State->Calls.push_back("leave " + context.GetRequest().Method);
		}

		void TestMethodHost::FinishRequest(MethodContext& context)
		{
			m_State->Calls.push_back("finish " + context.GetRequest().Method);
		}

		MetaState TestMethodHost::GetMetaState() const
		{
			return MetaState{ .Revision = m_State->Revision, .Dirty = false, .UndoLabel = {}, .Tick = std::nullopt, .PlayState = "Edit" };
		}

		std::string TestMethodHost::GetOffloadServerTag() const
		{
			return std::string(TestOffloadServerTag);
		}

		Result<std::string> TestMethodHost::WriteOffloadedResult(std::string_view fileName, std::string_view text)
		{
			m_State->Offloaded.emplace_back(std::string(fileName), std::string(text));
			return "/offload/" + std::string(fileName);
		}

		Scope<TypeRegistry> CreateProtocolTestTypes()
		{
			Scope<TypeRegistry> types = CreateScope<TypeRegistry>();
			RegisterBuiltinComponents(*types);
			types->Enum<TestShape>("TestShape", "A shape of the protocol tests.")
				.Entry(TestShape::Circle, "Circle", "Round.")
				.Entry(TestShape::Square, "Square", "Four corners.");
			types->Struct<EchoParams>("TestEchoParams", "The params of test.echo.")
				.Field("text", &EchoParams::Text, "The text to echo.")
				.Field("count", &EchoParams::Count, "How often.", { .Min = 0.0, .Max = 100.0 })
				.Field("shape", &EchoParams::Shape, "A shape.")
				.VariantField("components", &EchoParams::Components, "Component values by registry name.", &ResolveComponentValue);
			types->Struct<EchoResult>("TestEchoResult", "The result of test.echo.")
				.Field("text", &EchoResult::Text, "The text.")
				.Field("count", &EchoResult::Count, "How often.")
				.Field("shape", &EchoResult::Shape, "The shape.")
				.Field("componentNames", &EchoResult::ComponentNames, "The component names given.");
			// A struct without fields has no field to chain to the builder, so the builder is discarded explicitly.
			static_cast<void>(types->Struct<ReadParams>("TestReadParams", "No params."));
			types->Struct<ReadResult>("TestReadResult", "The result of test.read.").Field("revision", &ReadResult::Revision, "The host's revision.");
			types->Struct<LargeParams>("TestLargeParams", "The params of test.large.").Field("count", &LargeParams::Count, "Items.");
			types->Struct<LargeResult>("TestLargeResult", "The result of test.large.").Field("items", &LargeResult::Items, "The items.");
			types->Struct<PendParams>("TestPendParams", "The params of test.pend.").Field("polls", &PendParams::Polls, "Polls to resolve.");
			types->Struct<PendResult>("TestPendResult", "The result of test.pend.").Field("polls", &PendResult::Polls, "Polls taken.");
			types->Freeze();
			return types;
		}

		void RegisterProtocolTestMethods(MethodRegistry& methods)
		{
			methods.Add<TestHostContext, EchoParams, EchoResult>(
				{
					.Name = "test.echo",
					.Description = "Echoes its params.",
					.RequiredParams = { "text" },
					.ExposeAsTool = true,
					.Mutates = true,
					.SupportsDryRun = true,
					.AllowedInBatch = true,
					.Examples = { MethodExample{ .Description = "Echo once.", .Params = Json{ { "text", "hello" } } } },
				},
				&Echo);
			methods.Add<TestHostContext, ReadParams, ReadResult>(
				{
					.Name = "test.read",
					.Description = "Reads the host's revision.",
					.AvailableInLauncher = true,
					.AllowedInBatch = true,
					.Examples = { MethodExample{ .Description = "Read.", .Params = Json::object() } },
				},
				&Read);
			methods.Add<TestHostContext, LargeParams, LargeResult>(
				{ .Name = "test.large", .Description = "A large result.", .Examples = { MethodExample{ .Description = "Small.", .Params = Json{ { "count", 1 } } } } },
				&Large);
			methods.AddPending<TestHostContext, PendParams, PendResult>(
				{ .Name = "test.pend", .Description = "Resolves after some polls.", .Examples = { MethodExample{ .Description = "Two polls.", .Params = Json{ { "polls", 2 } } } } },
				&Pend);
			methods.Add<TestHostContext, ReadParams, ReadResult>(
				{ .Name = "test.fail", .Description = "Always fails.", .AllowedInBatch = true, .Examples = { MethodExample{ .Description = "Fail.", .Params = Json::object() } } },
				&Fail);
			methods.Add<TestHostContext, ReadParams, ReadResult>(
				{ .Name = "test.throw", .Description = "Lets an exception escape.", .Examples = { MethodExample{ .Description = "Throw.", .Params = Json::object() } } },
				&Throw);
			methods.Add<TestHostContext, ReadParams, ReadResult>(
				{ .Name = "test.systemError",
					.Description = "Lets a std::system_error escape.",
					.Examples = { MethodExample{ .Description = "Throw a system error.", .Params = Json::object() } } },
				&ThrowSystemError);
		}

		RpcRequest MakeTestRequest(int64_t id, std::string method, Json params)
		{
			return RpcRequest{
				.Id = Json(id),
				.IsNotification = false,
				.Method = std::move(method),
				.Params = std::move(params),
				.TranscriptLine = std::nullopt,
			};
		}

	}

}
