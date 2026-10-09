#include "TestsPCH.h"

#include "Shared/ShadowConstants.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/ShaderLibrary.h"

#include <array>
#include <cstddef>

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: shared constants match Slang reflection")
		{
			VirtualFileSystem vfs;
			auto mount = NativeDirectoryMount::Create(std::filesystem::path(ENGINE_SHADER_DIRECTORY), MountAccess::ReadOnly);
			REQUIRE(mount);
			REQUIRE(vfs.Mount(ShaderScheme, std::move(*mount)));
			auto path = VfsPath::Create(ShaderScheme, "");
			REQUIRE(path);
			ShaderLibrary shaders(nullptr, vfs, *path);
			const std::array<ShaderDefine, 1> permutation = { ShaderDefine{ .Key = "ALPHA_MASK", .Value = "0" } };
			auto reflection = shaders.GetReflection("Scene", "PSForward", permutation);
			REQUIRE(reflection);
			const auto* shadows = (*reflection)->FindStruct("ShadowConstants");
			const auto* spot = (*reflection)->FindStruct("SpotShadowConstants");
			REQUIRE(shadows != nullptr);
			REQUIRE(spot != nullptr);
			CHECK(shadows->Size == sizeof(ShadowConstants));
			CHECK(spot->Size == sizeof(SpotShadowConstants));
#define ENGINE_SHADOW_FIELD(type, field) ShaderStructField{ #field, static_cast<uint32_t>(offsetof(type, field)), static_cast<uint32_t>(sizeof(type::field)) }
			const std::array<ShaderStructField, 8> shadowFields = {
				ENGINE_SHADOW_FIELD(ShadowConstants, CascadeViewProjection), ENGINE_SHADOW_FIELD(ShadowConstants, CascadeSplits),
				ENGINE_SHADOW_FIELD(ShadowConstants, CascadeBlendStarts), ENGINE_SHADOW_FIELD(ShadowConstants, CascadeTexelWorldSizes),
				ENGINE_SHADOW_FIELD(ShadowConstants, CascadeDepthRanges), ENGINE_SHADOW_FIELD(ShadowConstants, Spots),
				ENGINE_SHADOW_FIELD(ShadowConstants, Directional), ENGINE_SHADOW_FIELD(ShadowConstants, Counts)
			};
			const std::array<ShaderStructField, 5> spotFields = {
				ENGINE_SHADOW_FIELD(SpotShadowConstants, ViewProjection), ENGINE_SHADOW_FIELD(SpotShadowConstants, UvScaleBias),
				ENGINE_SHADOW_FIELD(SpotShadowConstants, DepthSoftness), ENGINE_SHADOW_FIELD(SpotShadowConstants, Bias),
				ENGINE_SHADOW_FIELD(SpotShadowConstants, Indices)
			};
#undef ENGINE_SHADOW_FIELD
			const auto compare = [](const ShaderStruct& actual, std::span<const ShaderStructField> expected)
			{
				REQUIRE(actual.Fields.size() == expected.size());
				for (size_t index = 0; index < expected.size(); ++index)
				{
					CAPTURE(expected[index].Name);
					CHECK(actual.Fields[index].Name == expected[index].Name);
					CHECK(actual.Fields[index].Offset == expected[index].Offset);
					CHECK(actual.Fields[index].Size == expected[index].Size);
				}
			};
			compare(*shadows, shadowFields);
			compare(*spot, spotFields);
		}
	}

}
