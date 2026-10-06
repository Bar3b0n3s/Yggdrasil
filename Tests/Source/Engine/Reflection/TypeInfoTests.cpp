#include "TestsPCH.h"

#include "Engine/Reflection/TypeInfo.h"

#include "Engine/Asset/TypedAssetHandle.h"
#include "Support/ReflectionTestTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeInfo: TypeKeyOf is distinct per type and ignores cv and reference qualifiers")
		{
			CHECK(TypeKeyOf<int32_t>() != nullptr);
			CHECK(TypeKeyOf<int32_t>() != TypeKeyOf<uint32_t>());
			CHECK(TypeKeyOf<glm::vec3>() != TypeKeyOf<glm::vec4>());
			CHECK(TypeKeyOf<const glm::vec3&>() == TypeKeyOf<glm::vec3>());
			CHECK(TypeKeyOf<volatile std::string>() == TypeKeyOf<std::string>());
			// A colour shares its C++ type with the vector but has its own key (FieldBuilder::ColorField).
			CHECK(TypeKeyOf<Detail::ColorOf<glm::vec3>>() != TypeKeyOf<glm::vec3>());
		}

		TEST_CASE("TypeInfo: DeduceFieldType maps every supported C++ type to its kind")
		{
			static_assert(Detail::DeduceFieldType<bool>() == FieldType::Bool);
			static_assert(Detail::DeduceFieldType<int32_t>() == FieldType::Int32);
			static_assert(Detail::DeduceFieldType<uint32_t>() == FieldType::UInt32);
			static_assert(Detail::DeduceFieldType<float>() == FieldType::Float);
			static_assert(Detail::DeduceFieldType<glm::vec2>() == FieldType::Vec2);
			static_assert(Detail::DeduceFieldType<glm::vec3>() == FieldType::Vec3);
			static_assert(Detail::DeduceFieldType<glm::vec4>() == FieldType::Vec4);
			static_assert(Detail::DeduceFieldType<glm::quat>() == FieldType::Quat);
			static_assert(Detail::DeduceFieldType<glm::bvec3>() == FieldType::Bool3);
			static_assert(Detail::DeduceFieldType<std::string>() == FieldType::String);
			static_assert(Detail::DeduceFieldType<UUID>() == FieldType::EntityRef);
			static_assert(Detail::DeduceFieldType<TypedAssetHandle<AssetType::Mesh>>() == FieldType::AssetRef);
			static_assert(Detail::DeduceFieldType<Test::TestShape>() == FieldType::Enum);
			static_assert(Detail::DeduceFieldType<std::vector<std::string>>() == FieldType::Array);
			static_assert(Detail::DeduceFieldType<std::map<std::string, float>>() == FieldType::Map);
			static_assert(Detail::DeduceFieldType<Test::TestInner>() == FieldType::Struct);
			static_assert(Detail::DeduceFieldType<VariantValue>() == FieldType::Variant);
			CHECK(Detail::DeduceFieldType<std::map<std::string, VariantValue>>() == FieldType::Map);
		}

		TEST_CASE("TypeInfo: array operations resize, index, copy and clear a std::vector")
		{
			const TypeOps ops = Detail::MakeTypeOps<std::vector<int32_t>>();
			CHECK(ops.Read == nullptr);
			CHECK(ops.VisitEntries == nullptr);

			const ObjectPtr array = ops.Create();
			REQUIRE(array != nullptr);
			CHECK(ops.GetCount(array.get()) == 0u);
			ops.Resize(array.get(), 3);
			CHECK(ops.GetCount(array.get()) == 3u);
			*static_cast<int32_t*>(ops.GetElement(array.get(), 2)) = 7;
			CHECK(*static_cast<const int32_t*>(ops.GetConstElement(array.get(), 0)) == 0); // new elements are default-constructed
			CHECK(*static_cast<const int32_t*>(ops.GetConstElement(array.get(), 2)) == 7);

			const ObjectPtr copy = ops.Create();
			ops.Copy(copy.get(), array.get());
			CHECK(*static_cast<const std::vector<int32_t>*>(copy.get()) == std::vector<int32_t>{ 0, 0, 7 });

			ops.Clear(array.get());
			CHECK(ops.GetCount(array.get()) == 0u);
			ops.Reset(copy.get());
			CHECK(static_cast<const std::vector<int32_t>*>(copy.get())->empty());
		}

		TEST_CASE("TypeInfo: map operations insert, find, erase and visit in byte-wise key order")
		{
			const TypeOps ops = Detail::MakeTypeOps<std::map<std::string, float>>();
			CHECK(ops.Resize == nullptr);
			CHECK(ops.Write == nullptr);

			const ObjectPtr map = ops.Create();
			REQUIRE(map != nullptr);
			*static_cast<float*>(ops.FindOrInsert(map.get(), "zeta")) = 3.0f;
			*static_cast<float*>(ops.FindOrInsert(map.get(), "Alpha")) = 1.0f;
			CHECK(*static_cast<const float*>(ops.FindOrInsert(map.get(), "zeta")) == 3.0f); // an existing key keeps its value
			CHECK(*static_cast<const float*>(ops.FindOrInsert(map.get(), "beta")) == 0.0f); // a new key gets a default value
			CHECK(ops.GetCount(map.get()) == 3u);

			CHECK(ops.Find(map.get(), "missing") == nullptr);
			REQUIRE(ops.Find(map.get(), "Alpha") != nullptr);
			CHECK(*static_cast<const float*>(ops.Find(map.get(), "Alpha")) == 1.0f);

			std::vector<std::string> keys;
			ops.VisitEntries(map.get(), &keys, [](void* context, const std::string& key, const void* /*value*/)
			{
				static_cast<std::vector<std::string>*>(context)->push_back(key);
			});
			CHECK(keys == std::vector<std::string>{ "Alpha", "beta", "zeta" }); // uppercase sorts first

			CHECK(ops.Erase(map.get(), "beta"));
			CHECK_FALSE(ops.Erase(map.get(), "beta"));
			CHECK(ops.GetCount(map.get()) == 2u);
			ops.Clear(map.get());
			CHECK(ops.GetCount(map.get()) == 0u);
		}

		TEST_CASE("TypeInfo: struct operations create, copy and reset an object")
		{
			const TypeOps ops = Detail::MakeTypeOps<Test::TestInner>();
			CHECK(ops.Read == nullptr);
			CHECK(ops.GetCount == nullptr);

			const ObjectPtr object = ops.Create();
			REQUIRE(object != nullptr);
			Test::TestInner& inner = *static_cast<Test::TestInner*>(object.get());
			CHECK(inner.Label == "Inner");
			inner.Weight = 5.0f;
			inner.Label = "Changed";

			const ObjectPtr copy = ops.Create();
			ops.Copy(copy.get(), object.get());
			CHECK(static_cast<const Test::TestInner*>(copy.get())->Label == "Changed");
			CHECK(static_cast<const Test::TestInner*>(copy.get())->Weight == 5.0f);

			ops.Reset(object.get());
			CHECK(inner.Label == "Inner");
			CHECK(inner.Weight == 1.0f);
		}

		TEST_CASE("TypeInfo: accessors return the specification and a schema-only type has no operations")
		{
			TypeInfo::Specification elementSpecification;
			elementSpecification.Kind = FieldType::String;
			elementSpecification.Name = "String";
			elementSpecification.Key = TypeKeyOf<std::string>();
			elementSpecification.Ops = Detail::MakeTypeOps<std::string>();
			const TypeInfo element(std::move(elementSpecification));

			TypeInfo::Specification arraySpecification;
			arraySpecification.Kind = FieldType::Array;
			arraySpecification.Name = "Array";
			arraySpecification.Element = &element;
			arraySpecification.Key = TypeKeyOf<std::vector<std::string>>();
			arraySpecification.Ops = Detail::MakeTypeOps<std::vector<std::string>>();
			const TypeInfo array(std::move(arraySpecification));

			CHECK(array.GetKind() == FieldType::Array);
			CHECK(array.GetName() == "Array");
			CHECK(array.GetElement() == &element);
			CHECK(array.GetEnum() == nullptr);
			CHECK(array.GetStruct() == nullptr);
			CHECK(array.GetKey() == TypeKeyOf<std::vector<std::string>>());
			CHECK(array.HasOps());
			CHECK(array.GetOps().Resize != nullptr);
			CHECK(element.GetOps().Read != nullptr);

			// A script field schema describes JSON only (M13).
			TypeInfo::Specification schemaSpecification;
			schemaSpecification.Kind = FieldType::AssetRef;
			schemaSpecification.Name = "AssetRef";
			schemaSpecification.AssetTypeName = "AudioClip";
			const TypeInfo schemaOnly(std::move(schemaSpecification));
			CHECK_FALSE(schemaOnly.HasOps());
			CHECK(schemaOnly.GetKey() == nullptr);
			CHECK(schemaOnly.GetAssetTypeName() == "AudioClip");
		}

		TEST_CASE("TypeInfo: scalar operations read and write through Value")
		{
			const TypeOps number = Detail::MakeTypeOps<float>();
			const ObjectPtr mass = number.Create();
			number.Write(mass.get(), Value::FromFloat(2.5f));
			CHECK(*static_cast<const float*>(mass.get()) == 2.5f);
			CHECK(number.Read(mass.get()) == Value::FromFloat(2.5f));

			const TypeOps colour = Detail::MakeTypeOps<glm::vec3, FieldType::Color3>();
			const ObjectPtr tint = colour.Create();
			CHECK(colour.Read(tint.get()).GetKind() == FieldType::Color3); // not Vec3

			const TypeOps shape = Detail::MakeTypeOps<Test::TestShape>();
			const ObjectPtr value = shape.Create();
			shape.Write(value.get(), Value::FromEnum(1));
			CHECK(*static_cast<const Test::TestShape*>(value.get()) == Test::TestShape::Sphere);
			CHECK(shape.Read(value.get()).AsEnum() == 1);

			const TypeOps asset = Detail::MakeTypeOps<TypedAssetHandle<AssetType::Mesh>>();
			const ObjectPtr mesh = asset.Create();
			asset.Write(mesh.get(), Value::FromAssetRef(UUID(0x102)));
			CHECK(static_cast<const TypedAssetHandle<AssetType::Mesh>*>(mesh.get())->GetHandle() == AssetHandle(0x102));
			CHECK(asset.Read(mesh.get()).GetKind() == FieldType::AssetRef);
		}
	}

}
