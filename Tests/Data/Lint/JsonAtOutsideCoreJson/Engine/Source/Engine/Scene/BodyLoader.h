#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace Engine {

	struct LoadedBody
	{
		float Mass = 0.0f;
		std::string Name;
	};

	LoadedBody LoadBody(const nlohmann::json& document);

	float LoadScaledMass(const std::string& text);

}
