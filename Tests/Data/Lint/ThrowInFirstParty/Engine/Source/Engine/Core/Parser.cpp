#include "Engine/Core/Parser.h"

#include <stdexcept>

namespace Engine {

	int ParseDigit(char character)
	{
		if (character < '0' || character > '9')
			throw std::invalid_argument("not a digit"); // Seeded defect: first-party code never throws.
		return character - '0';
	}

	void ForwardError(const std::exception_ptr& error)
	{
		std::rethrow_exception(error); // Seeded defect: rethrowing an exception is throwing.
	}

}
