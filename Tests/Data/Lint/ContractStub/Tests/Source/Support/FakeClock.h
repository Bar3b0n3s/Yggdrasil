#pragma once

namespace Engine {

	namespace Test {

		class FakeClock
		{
		public:
			[[nodiscard]] double Now() const;
		};

	}

}
