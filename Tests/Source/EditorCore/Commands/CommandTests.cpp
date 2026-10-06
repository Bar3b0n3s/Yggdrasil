#include "TestsPCH.h"

#include "EditorCore/Commands/Command.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CommandOrigin: names both origins")
		{
			CHECK(CommandOriginToString(CommandOrigin::User) == "User");
			CHECK(CommandOriginToString(CommandOrigin::Agent) == "Agent");
		}
	}

}
