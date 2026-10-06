#include "TestsPCH.h"

#include "Engine/Reflection/ValidationContext.h"

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("ValidationContext: issues are located below the base pointer and pushed keys")
		{
			ValidationContext context("/components/Transform");
			context.Error("Scale", "must have a magnitude of at least 0.0001");
			context.PushKey("Inner");
			context.Warning("a/b", "escaped key");
			context.PopKey();
			context.Error("", "the object itself");

			REQUIRE(context.GetIssues().size() == 3);
			CHECK(context.GetIssues()[0].JsonPointer == "/components/Transform/Scale");
			CHECK(context.GetIssues()[1].JsonPointer == "/components/Transform/Inner/a~1b");
			CHECK(context.GetIssues()[1].Severity == DiagnosticSeverity::Warning);
			CHECK(context.GetIssues()[2].JsonPointer == "/components/Transform");
			CHECK(context.GetErrorCount() == 2);
			CHECK(context.GetPointer() == "/components/Transform");
		}

		TEST_CASE("ValidationContext: ToStatus fails only on errors and carries every error as an issue")
		{
			ValidationContext clean;
			clean.Warning("Mass", "unusual");
			CHECK(clean.ToStatus("RigidBody").has_value());

			ValidationContext context;
			context.Error("Mass", "must be > 0");
			context.Error("Friction", "must be >= 0");
			const Status status = context.ToStatus("RigidBody");
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(status.error().GetMessageText() == "2 invalid fields in RigidBody");
			REQUIRE(status.error().GetIssues().size() == 2);
			CHECK(status.error().GetIssues()[1].JsonPointer == "/Friction");
		}

		TEST_CASE("ValidationContext: one error keeps its own message and the status is located at the base pointer")
		{
			ValidationContext context("/components/RigidBody");
			context.PushKey("Inner");
			ValidationIssue issue;
			issue.JsonPointer = "/elsewhere";
			issue.Message = "absolute";
			issue.Hint = "did you mean 'Mass'?";
			issue.Suggestions = { "Mass" };
			context.AddIssue(issue);

			const Status status = context.ToStatus("RigidBody");
			context.PopKey();
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetMessageText() == "absolute");
			CHECK(status.error().GetLocation().JsonPointer == "/components/RigidBody");
			REQUIRE(status.error().GetIssues().size() == 1);
			CHECK(status.error().GetIssues()[0].JsonPointer == "/elsewhere"); // AddIssue keeps an absolute pointer
			CHECK(status.error().GetIssues()[0].Suggestions == std::vector<std::string>{ "Mass" });
		}

		TEST_CASE("ValidationContext: TakeIssues empties the context and nested keys unwind")
		{
			ValidationContext context;
			context.PushKey("A");
			context.PushKey("B");
			CHECK(context.GetPointer() == "/A/B");
			context.Warning("", "here");
			context.PopKey();
			CHECK(context.GetPointer() == "/A");
			context.PopKey();
			CHECK(context.GetPointer().empty());

			CHECK_FALSE(context.HasErrors());
			const std::vector<ValidationIssue> issues = context.TakeIssues();
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].JsonPointer == "/A/B");
			CHECK(context.GetIssues().empty());
			CHECK(context.ToStatus("Nothing").has_value());
		}
	}

}
