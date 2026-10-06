#include "TestsPCH.h"

#include "Engine/Core/VfsPath.h"

#include <unordered_set>

namespace Engine {

	static VfsPath ParseOrFail(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE_MESSAGE(path.has_value(), "expected a valid VFS path: ", text);
		return std::move(*path);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("VfsPath: rejects parent escapes, absolute paths, NUL and reserved names" * doctest::skip(true))
		{
			using namespace std::string_view_literals;
			const std::array rejected = {
				"project://../Secrets.txt"sv,
				"project://Assets/../../Escape.txt"sv,
				"project://Assets/./Level1.scene"sv,
				"project:///Assets/Level1.scene"sv,
				"project://C:/Windows/System32"sv,
				"project://Assets\\Level1.scene"sv,
				"project://Assets//Level1.scene"sv,
				"project://Assets/"sv,
				"project://Assets/Level\x00.scene"sv,
				"project://Assets/Tab\tName.scene"sv,
				"project://Assets/CON"sv,
				"project://Assets/nul.txt"sv,
				"project://Assets/Com1.json"sv,
				"project://Assets/LPT9"sv,
				"project://Assets/aux.prefab"sv,
				"project://Assets/nul.tar.gz"sv,
				"project://Assets/CON .txt"sv,
				"project://Assets/Lpt1  .scene.meta"sv,
				"project://Assets/Trailing."sv,
				"project://Assets/Trailing "sv,
				"project://Assets/Question?.scene"sv,
				"project://Assets/Star*.scene"sv,
				"project://Assets/\"Quoted\""sv,
				"project://Assets/Pipe|.scene"sv,
				"project://Assets/<Angle>.scene"sv,
				"project://Assets/Invalid\xff.scene"sv,
			};
			for (const std::string_view text : rejected)
			{
				const Result<VfsPath> path = VfsPath::Parse(text);
				INFO("path: ", std::string(text));
				REQUIRE_FALSE(path.has_value());
				CHECK(path.error().GetCode() == ErrorCode::Validation);
			}

			CHECK(VfsPath::ValidateRelativePath("Assets/Scenes/Level1.scene").has_value());
			CHECK_FALSE(VfsPath::ValidateRelativePath("../Level1.scene").has_value());
			CHECK_FALSE(VfsPath::ValidateRelativePath("/Level1.scene").has_value());

			// Names that merely start with a device name are fine.
			CHECK(VfsPath::ValidateRelativePath("Assets/Console.luau").has_value());
			CHECK(VfsPath::ValidateRelativePath("Assets/NULL.txt").has_value());
			CHECK(VfsPath::ValidateRelativePath("Assets/COM10.json").has_value());
			CHECK(VfsPath::ValidateRelativePath("Assets/.nul").has_value());
		}

		TEST_CASE("VfsPath: accepts project paths and keeps their spelling" * doctest::skip(true))
		{
			const VfsPath path = ParseOrFail("project://Assets/Scenes/Level1.scene");
			CHECK(path.GetScheme() == "project");
			CHECK(path.GetPath() == "Assets/Scenes/Level1.scene");
			CHECK(path.ToString() == "project://Assets/Scenes/Level1.scene");
			CHECK(std::format("{}", path) == "project://Assets/Scenes/Level1.scene");
			CHECK_FALSE(path.IsRoot());
			CHECK_FALSE(path.IsEmpty());

			const VfsPath root = ParseOrFail("engine://");
			CHECK(root.IsRoot());
			CHECK(root.GetPath().empty());

			CHECK(ParseOrFail("user://Settings/Editor.json").GetPath() == "Settings/Editor.json");
			CHECK(ParseOrFail("project://Assets/Caf\xc3\xa9 Ol\xc3\xa9.png").GetFileName() == "Caf\xc3\xa9 Ol\xc3\xa9.png");
			CHECK(ParseOrFail("project://.luaurc").GetFileName() == ".luaurc");
			CHECK(VfsPath().IsEmpty());
			CHECK(VfsPath().ToString().empty());
		}

		TEST_CASE("VfsPath: schemes are lowercase letters followed by ://" * doctest::skip(true))
		{
			CHECK(VfsPath::ValidateScheme("enginecache").has_value());
			CHECK_FALSE(VfsPath::ValidateScheme("").has_value());
			CHECK_FALSE(VfsPath::ValidateScheme("Project").has_value());
			CHECK_FALSE(VfsPath::ValidateScheme("cache2").has_value());
			CHECK_FALSE(VfsPath::Parse("Assets/Level1.scene").has_value());
			CHECK_FALSE(VfsPath::Parse("project:/Assets").has_value());
			CHECK_FALSE(VfsPath::Parse("://Assets").has_value());

			const Result<VfsPath> created = VfsPath::Create("cache", "1234/abcd.bin");
			REQUIRE(created.has_value());
			CHECK(created->ToString() == "cache://1234/abcd.bin");
			CHECK_FALSE(VfsPath::Create("cache", "../x").has_value());
		}

		TEST_CASE("VfsPath: GetFileName, GetStem, GetExtension and GetParent split the path" * doctest::skip(true))
		{
			const VfsPath meta = ParseOrFail("project://Assets/Models/Track.glb.meta");
			CHECK(meta.GetFileName() == "Track.glb.meta");
			CHECK(meta.GetExtension() == ".meta");
			CHECK(meta.GetStem() == "Track.glb");
			CHECK(meta.GetParent() == ParseOrFail("project://Assets/Models"));
			CHECK(meta.GetParent().GetParent().GetParent().IsRoot());
			CHECK(ParseOrFail("project://").GetParent().IsRoot());

			const VfsPath dotFile = ParseOrFail("project://.luaurc");
			CHECK(dotFile.GetExtension().empty());
			CHECK(dotFile.GetStem() == ".luaurc");

			const VfsPath noExtension = ParseOrFail("project://Assets/README");
			CHECK(noExtension.GetExtension().empty());
			CHECK(noExtension.GetStem() == "README");
		}

		TEST_CASE("VfsPath: Join validates every segment" * doctest::skip(true))
		{
			const VfsPath assets = ParseOrFail("project://Assets");
			const Result<VfsPath> scene = assets.Join("Scenes/Level1.scene");
			REQUIRE(scene.has_value());
			CHECK(scene->ToString() == "project://Assets/Scenes/Level1.scene");

			const Result<VfsPath> fromRoot = ParseOrFail("project://").Join("Game.eproj");
			REQUIRE(fromRoot.has_value());
			CHECK(fromRoot->ToString() == "project://Game.eproj");

			CHECK_FALSE(assets.Join("../Escape").has_value());
			CHECK_FALSE(assets.Join("").has_value());
			CHECK_FALSE(assets.Join("A//B").has_value());

			const Result<VfsPath> fromEmpty = VfsPath().Join("A");
			REQUIRE_FALSE(fromEmpty.has_value());
			CHECK(fromEmpty.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("VfsPath: IsUnder compares whole segments" * doctest::skip(true))
		{
			const VfsPath scenes = ParseOrFail("project://Assets/Scenes");
			CHECK(ParseOrFail("project://Assets/Scenes/Level1.scene").IsUnder(scenes));
			CHECK(scenes.IsUnder(scenes));
			CHECK_FALSE(ParseOrFail("project://Assets/Scenes2/Level1.scene").IsUnder(scenes));
			CHECK_FALSE(ParseOrFail("engine://Assets/Scenes/Level1.scene").IsUnder(scenes));
			CHECK(scenes.IsUnder(ParseOrFail("project://")));
		}

		TEST_CASE("VfsPath: ordering is byte-wise on the scheme, then the path" * doctest::skip(true))
		{
			std::vector<VfsPath> paths = {
				ParseOrFail("project://b.txt"),
				ParseOrFail("engine://z.txt"),
				ParseOrFail("project://B.txt"),
				ParseOrFail("project://a/b.txt"),
			};
			std::ranges::sort(paths);
			CHECK(paths[0].ToString() == "engine://z.txt");
			CHECK(paths[1].ToString() == "project://B.txt");
			CHECK(paths[2].ToString() == "project://a/b.txt");
			CHECK(paths[3].ToString() == "project://b.txt");

			CHECK(ParseOrFail("project://A.txt") != ParseOrFail("project://a.txt"));
			const std::unordered_set<VfsPath> set = { ParseOrFail("user://x"), ParseOrFail("user://x"), ParseOrFail("cache://x") };
			CHECK(set.size() == 2);
		}
	}

}
