#include "TestsPCH.h"
#include "Support/TempDirectory.h"

// M1 contract stub (Roadmap rule 3): stream E implements creation and removal. The stub stops the test case, so no
// test ever writes relative to an empty path.

namespace Engine {

	namespace Test {

		TempDirectory::TempDirectory(std::string_view /*label*/)
		{
			FAIL("Test::TempDirectory is an M1 contract stub");
		}

		TempDirectory::~TempDirectory() = default;

		std::filesystem::path TempDirectory::operator/(std::string_view /*relative*/) const
		{
			return m_Path;
		}

	}

}
