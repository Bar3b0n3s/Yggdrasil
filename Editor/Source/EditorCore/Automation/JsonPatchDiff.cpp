#include "EditorPCH.h"
#include "EditorCore/Automation/JsonPatchDiff.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		static Json MakePatchOperation(std::string_view op, const std::string& path)
		{
			Json operation = Json::object();
			operation["op"] = std::string(op);
			operation["path"] = path;
			return operation;
		}

		static Json MakePatchOperation(std::string_view op, const std::string& path, const Json& value)
		{
			Json operation = MakePatchOperation(op, path);
			operation["value"] = value;
			return operation;
		}

		// Appends to `patch` the operations that turn `from` into `to`, both located at `path`. The recursion follows the
		// documents' nesting, which JsonReader::Parse bounds at MaxJsonDepth.
		static void AppendDiff(const Json& from, const Json& to, const std::string& path, Json& patch)
		{
			if (from.is_object() && to.is_object())
			{
				for (auto member = from.begin(); member != from.end(); ++member)
				{
					if (to.find(member.key()) == to.end())
						patch.push_back(MakePatchOperation("remove", JsonReader::AppendPointer(path, member.key())));
				}
				for (auto member = to.begin(); member != to.end(); ++member)
				{
					const std::string memberPath = JsonReader::AppendPointer(path, member.key());
					const auto source = from.find(member.key());
					if (source == from.end())
						patch.push_back(MakePatchOperation("add", memberPath, member.value()));
					else
						AppendDiff(source.value(), member.value(), memberPath, patch);
				}
				return;
			}

			if (from.is_array() && to.is_array() && from.size() == to.size())
			{
				for (size_t index = 0; index < from.size(); ++index)
					AppendDiff(from[index], to[index], JsonReader::AppendPointer(path, index), patch);
				return;
			}

			if (from != to)
				patch.push_back(MakePatchOperation("replace", path, to));
		}

	}

	Json DiffJson(const Json& from, const Json& to)
	{
		Json patch = Json::array();
		Utils::AppendDiff(from, to, std::string(), patch);
		return patch;
	}

}
