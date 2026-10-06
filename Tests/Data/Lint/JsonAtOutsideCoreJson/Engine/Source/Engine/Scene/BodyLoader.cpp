#include "Engine/Scene/BodyLoader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	LoadedBody LoadBody(const nlohmann::json& document)
	{
		// Seeded defects: throwing json accessors outside Core/Json instead of JsonReader.
		LoadedBody body;
		body.Mass = document.at("Mass").get<float>();
		body.Name = document["Name"].get<std::string>();
		return body;
	}

	using Json = nlohmann::json;

	static nlohmann::json ParseDocument(const std::string& text)
	{
		return nlohmann::json::parse(text, nullptr, false);
	}

	float LoadScaledMass(const std::string& text)
	{
		// Seeded defects: the same accessors on a receiver declared through a type alias, on a value returned by a
		// function and held in auto, and on a call chain. Only the alias is visible without types (regex mode).
		const Json aliased = ParseDocument(text);
		const auto parsed = ParseDocument(text);
		const float aliasedMass = aliased.at("Mass").get<float>();
		const float parsedMass = parsed.at("Mass").get<float>();
		const float scale = ParseDocument(text).at("Scale").get<float>();
		return (aliasedMass + parsedMass) * scale;
	}

}
