#include "TestsPCH.h"
#include "Support/DefaultFields.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Engine {

	namespace Test {

		void CollectDefaultFields(const StructInfo& type, const std::vector<const Json*>& actuals, const Json& defaults,
			const std::string& pointer, std::vector<std::string>& uncovered)
		{
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (!field->GetMeta().Serialized)
					continue;
				const std::string& name = field->GetName();
				const std::string fieldPointer = JsonReader::AppendPointer(pointer, name);
				const TypeInfo& fieldType = field->GetType();

				if (fieldType.GetKind() == FieldType::Struct)
				{
					std::vector<const Json*> nested;
					for (const Json* actual : actuals)
					{
						if (actual->contains(name))
							nested.push_back(&(*actual)[name]);
					}
					CollectDefaultFields(*fieldType.GetStruct(), nested, defaults[name], fieldPointer, uncovered);
					continue;
				}

				const TypeInfo* element = fieldType.GetElement();
				if (element != nullptr && element->GetKind() == FieldType::Struct)
				{
					std::vector<const Json*> elements;
					for (const Json* actual : actuals)
					{
						if (!actual->contains(name))
							continue;
						for (const Json& value : (*actual)[name])
							elements.push_back(&value);
					}
					if (elements.empty())
						uncovered.push_back(fieldPointer);
					else
						CollectDefaultFields(*element->GetStruct(), elements, element->GetStruct()->MakeDefaultJson(), fieldPointer + "/*", uncovered);
					continue;
				}

				const bool covered = std::any_of(actuals.begin(), actuals.end(), [&](const Json* actual)
				{
					return actual->contains(name) && (*actual)[name] != defaults[name];
				});
				if (!covered)
					uncovered.push_back(fieldPointer);
			}
		}

	}

}
