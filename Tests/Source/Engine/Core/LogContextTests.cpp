#include "TestsPCH.h"

#include "Engine/Core/LogContext.h"

#include <thread>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("LogContextScope: nested scopes restore the previous context")
		{
			CHECK_FALSE(LogContextScope::GetCurrent().Tick.has_value());

			LogContext outer;
			outer.Tick = 10;
			{
				LogContextScope outerScope(outer);
				CHECK(LogContextScope::GetCurrent().Tick == 10);

				LogContext inner = LogContextScope::GetCurrent();
				inner.Entity = UUID(42);
				inner.ScriptFile = "Assets/Scripts/Ball.luau";
				inner.ScriptLine = 3;
				{
					LogContextScope innerScope(inner);
					CHECK(LogContextScope::GetCurrent().Tick == 10);
					CHECK(LogContextScope::GetCurrent().Entity == UUID(42));
					CHECK(LogContextScope::GetCurrent().ScriptFile == "Assets/Scripts/Ball.luau");
					CHECK(LogContextScope::GetCurrent().ScriptLine == 3);
				}

				CHECK_FALSE(LogContextScope::GetCurrent().Entity.IsValid());
				CHECK(LogContextScope::GetCurrent().ScriptFile.empty());
			}

			CHECK_FALSE(LogContextScope::GetCurrent().Tick.has_value());
		}

		TEST_CASE("LogContextScope: the context belongs to the calling thread")
		{
			LogContext context;
			context.Entity = UUID(7);
			LogContextScope scope(context);

			bool otherThreadSawEntity = true;
			std::thread other([&otherThreadSawEntity]()
			{
				otherThreadSawEntity = LogContextScope::GetCurrent().Entity.IsValid();
			});
			other.join();

			CHECK_FALSE(otherThreadSawEntity);
			CHECK(LogContextScope::GetCurrent().Entity == UUID(7));
		}
	}

}
