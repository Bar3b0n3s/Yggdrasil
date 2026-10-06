#include "TestsPCH.h"

// doctest guards its implementation separately from its interface, so including the header again after
// DOCTEST_CONFIG_IMPLEMENT compiles the test runner into this translation unit only.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

int main(int argc, char** argv)
{
	doctest::Context context;
	context.applyCommandLine(argc, argv);
	return context.run();
}
