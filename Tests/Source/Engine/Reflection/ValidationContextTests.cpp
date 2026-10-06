#include "TestsPCH.h"

#include "Engine/Reflection/ValidationContext.h"

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("ValidationContext: issues are located below the base pointer and pushed keys" * doctest::skip(true))
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

		TEST_CASE("ValidationContext: ToStatus fails only on errors and carries every error as an issue" * doctest::skip(true))
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
	}

}
