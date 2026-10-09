#pragma once

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/EditorMethods.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Automation/ScreenshotMethods.h"
#include "EditorCore/Automation/ViewportMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

namespace Engine::Test {

	// Exercises the production type and method registrations with a request-local context.
	class M10MethodFixture
	{
	public:
		explicit M10MethodFixture(const AutomationServerSpecification& specification = MakeTestServerSpecification())
			: Fixture("M10Methods"), Methods(Fixture.GetEditor().GetTypeRegistry())
		{
			Fixture.CreateAndOpenProject();
			Fixture.CreateAndOpenScene();
			Client = CreateScope<AutomationTestClient>(Fixture.GetEditor(), specification);
			RegisterEditorMethods(Methods, {});
			Methods.Freeze();
		}
		[[nodiscard]] Scope<EditorMethodContext> Context(std::string_view name, const Json& params = Json::object())
		{
			const MethodDescriptor* method = Methods.Find(name);
			REQUIRE(method != nullptr);
			const auto prepared = Methods.PrepareParams(*method, params);
			REQUIRE_MESSAGE(prepared.has_value(), prepared.error().ToString());
			MethodRequest request;
			request.Info.Client = Client->GetClient();
			request.Info.ClientName = "test";
			request.Info.Method = std::string(name);
			request.Method = method;
			request.Params = prepared->Params;
			request.Options = prepared->Options;
			request.Registry = &Methods;
			return CreateScope<EditorMethodContext>(Fixture.GetEditor(), Client->GetServer(), std::move(request));
		}
		[[nodiscard]] Result<Json> Call(std::string_view name, const Json& params = Json::object())
		{
			auto context = Context(name, params);
			MethodResult result = Methods.Invoke(*context);
			if (const auto error = std::get_if<Error>(&result))
				return std::unexpected(*error);
			REQUIRE(std::holds_alternative<Json>(result));
			return std::move(std::get<Json>(result));
		}
		Test::EditorTestFixture Fixture;
		Scope<AutomationTestClient> Client{};
		MethodRegistry Methods;
	};

}
