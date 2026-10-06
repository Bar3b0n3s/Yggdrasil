#pragma once

#include <exception>

namespace Engine {

	int ParseDigit(char character);

	void ForwardError(const std::exception_ptr& error);

}
