#pragma once

#include "Engine/Automation/Protocol/Dispatcher.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/VariantValue.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A host, methods and reflected structs for testing the protocol module without the editor: what the MethodRegistry,
// MethodContext and Dispatcher tests register and dispatch.

namespace Engine {

	namespace Test {

		// Registry enum "TestShape".
		enum class TestShape : uint8_t
		{
			Circle,
			Square
		};

		// test.echo {text, count?, shape?, components?}: registry "TestEchoParams" (count: Min 0, Max 100; components:
		// ResolveComponentValue).
		struct EchoParams
		{
			std::string Text{};
			int32_t Count = 1;
			TestShape Shape = TestShape::Circle;
			std::map<std::string, VariantValue> Components{};
		};

		// Registry "TestEchoResult": the params echoed, component maps by name.
		struct EchoResult
		{
			std::string Text{};
			int32_t Count = 0;
			TestShape Shape = TestShape::Circle;
			std::vector<std::string> ComponentNames{};
		};

		// test.large {count}: registry "TestLargeParams"; its result "TestLargeResult" holds `count` 64-character strings,
		// so 1000 of them exceed the offload threshold.
		struct LargeParams
		{
			uint32_t Count = 0;
		};

		struct LargeResult
		{
			std::vector<std::string> Items{};
		};

		// test.pend {polls}: registry "TestPendParams"; a pending operation that resolves on its `polls`-th Poll with
		// "TestPendResult" {polls}.
		struct PendParams
		{
			uint32_t Polls = 1;
		};

		struct PendResult
		{
			uint32_t Polls = 0;
		};

		// What the test host records and decides.
		struct TestHostState
		{
			// "available <method>", "admit <method>", "enter <method>", "handler <method>", "leave <method>",
			// "finish <method>", "cancel <method>", in call order.
			std::vector<std::string> Calls{};
			uint32_t Revision = 0; // reported in MetaState
			// CheckAvailability refuses methods without AvailableInLauncher (InvalidState), as in the launcher state.
			bool LauncherState = false;
			bool DenyEverything = false;                                  // AdmitRequest fails with PermissionDenied
			std::vector<std::pair<std::string, std::string>> Offloaded{}; // (file name, text)
		};

		// The host context of the test methods.
		class TestHostContext final : public MethodContext
		{
		public:
			TestHostContext(TestHostState& state, MethodRequest request);

			[[nodiscard]] TestHostState& GetState() const { return *m_State; }
			[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override;
		private:
			TestHostState* m_State = nullptr; // documented back-reference
		};

		// The offload server tag of TestMethodHost.
		inline constexpr std::string_view TestOffloadServerTag = "4242-1791244800";

		// An IMethodHost over a TestHostState, recording every call.
		class TestMethodHost final : public IMethodHost
		{
		public:
			explicit TestMethodHost(TestHostState& state);

			[[nodiscard]] Status CheckAvailability(const MethodDescriptor& method) const override;
			[[nodiscard]] Scope<MethodContext> CreateContext(MethodRequest request) override;
			[[nodiscard]] Status AdmitRequest(MethodContext& context) override;
			void EnterInvocation(MethodContext& context) override;
			void LeaveInvocation(MethodContext& context) override;
			void FinishRequest(MethodContext& context) override;
			[[nodiscard]] MetaState GetMetaState() const override;
			[[nodiscard]] std::string GetOffloadServerTag() const override;
			[[nodiscard]] Result<std::string> WriteOffloadedResult(std::string_view fileName, std::string_view text) override;
		private:
			TestHostState* m_State = nullptr; // documented back-reference
		};

		// A frozen registry with the built-in components and the test structs and enum above.
		[[nodiscard]] Scope<TypeRegistry> CreateProtocolTestTypes();

		// Registers test.echo (mutates, supports dry runs, requires "text", a tool, allowed in batches), test.read (no params,
		// read-only, available in the launcher state, allowed in batches), test.large, test.pend (pending), test.fail (NotFound
		// "nothing here", allowed in batches) and test.throw (an exception from the standard library escapes the handler).
		void RegisterProtocolTestMethods(MethodRegistry& methods);

		// {"jsonrpc": "2.0", "id": id, "method": method, "params": params} as a parsed RpcRequest.
		[[nodiscard]] RpcRequest MakeTestRequest(int64_t id, std::string method, Json params);

	}

}
