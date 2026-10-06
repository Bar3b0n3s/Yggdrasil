#include "TestsPCH.h"

#include "Engine/Core/Mounts/OverlayMount.h"

#include "Engine/Core/Mounts/MemoryMount.h"

namespace Engine {

	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	static VfsPath Path(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return std::move(*path);
	}

	static std::string ReadText(const IMount& mount, std::string_view path)
	{
		Result<Buffer> bytes = mount.ReadFile(Path(path));
		return bytes.has_value() ? std::string(AsStringView(*bytes)) : std::string("<missing>");
	}

	// A lower mount with Assets/Level1.scene and Assets/Old.scene.
	static Scope<MemoryMount> MakeLower()
	{
		Scope<MemoryMount> lower = CreateScope<MemoryMount>();
		REQUIRE(lower->CreateDirectories(Path("project://Assets")).has_value());
		REQUIRE(lower->WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("lower level")).has_value());
		REQUIRE(lower->WriteFileAtomic(Path("project://Assets/Old.scene"), AsBytes("old")).has_value());
		return lower;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("OverlayMount: writes stay in memory and never reach the lower mount" * doctest::skip(true))
		{
			Scope<MemoryMount> lowerOwner = MakeLower();
			const MemoryMount* lower = lowerOwner.get();
			const uint64_t lowerMutations = lower->GetMutationCount();
			OverlayMount overlay(std::move(lowerOwner));

			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("overlay level")).has_value());
			REQUIRE(overlay.CreateDirectories(Path("project://Assets/New")).has_value());
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/New/Created.prefab"), AsBytes("created")).has_value());
			REQUIRE(overlay.Move(Path("project://Assets/Old.scene"), Path("project://Assets/Renamed.scene")).has_value());

			CHECK(ReadText(overlay, "project://Assets/Level1.scene") == "overlay level");
			CHECK(ReadText(overlay, "project://Assets/New/Created.prefab") == "created");
			CHECK(ReadText(overlay, "project://Assets/Renamed.scene") == "old");
			CHECK(ErrorCodeOf(overlay.ReadFile(Path("project://Assets/Old.scene"))) == ErrorCode::NotFound);

			CHECK(lower->GetMutationCount() == lowerMutations);
			CHECK(ReadText(*lower, "project://Assets/Level1.scene") == "lower level");
			CHECK(ReadText(*lower, "project://Assets/Old.scene") == "old");
			CHECK(ErrorCodeOf(lower->GetInfo(Path("project://Assets/New"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(lower->GetInfo(Path("project://Assets/Renamed.scene"))) == ErrorCode::NotFound);

			const std::vector<std::string> expectedChanges = {
				"Assets/Level1.scene",
				"Assets/New",
				"Assets/New/Created.prefab",
				"Assets/Old.scene",
				"Assets/Renamed.scene",
			};
			CHECK(overlay.GetChangedPaths() == expectedChanges);
		}

		TEST_CASE("OverlayMount: removals hide lower files without touching them" * doctest::skip(true))
		{
			Scope<MemoryMount> lowerOwner = MakeLower();
			const MemoryMount* lower = lowerOwner.get();
			OverlayMount overlay(std::move(lowerOwner));

			REQUIRE(overlay.Remove(Path("project://Assets/Level1.scene")).has_value());
			CHECK(ErrorCodeOf(overlay.GetInfo(Path("project://Assets/Level1.scene"))) == ErrorCode::NotFound);
			CHECK(ReadText(*lower, "project://Assets/Level1.scene") == "lower level");

			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("recreated")).has_value());
			CHECK(ReadText(overlay, "project://Assets/Level1.scene") == "recreated");

			overlay.Clear();
			CHECK(ReadText(overlay, "project://Assets/Level1.scene") == "lower level");
			CHECK(overlay.GetChangedPaths().empty());
		}

		TEST_CASE("OverlayMount: List merges both layers in byte-wise order" * doctest::skip(true))
		{
			OverlayMount overlay(MakeLower());
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/A.scene"), AsBytes("a")).has_value());
			REQUIRE(overlay.Remove(Path("project://Assets/Old.scene")).has_value());

			const Result<std::vector<VfsEntry>> listed = overlay.List(Path("project://Assets"), false);
			REQUIRE(listed.has_value());
			std::vector<std::string> names;
			for (const VfsEntry& entry : *listed)
				names.push_back(std::string(entry.Path.GetFileName()));
			CHECK(names == std::vector<std::string>{ "A.scene", "Level1.scene" });
		}

		TEST_CASE("OverlayMount: overlays a read-only mount" * doctest::skip(true))
		{
			Scope<MemoryMount> lowerOwner = MakeLower();
			lowerOwner->SetAccess(MountAccess::ReadOnly);
			OverlayMount overlay(std::move(lowerOwner));

			CHECK(overlay.GetAccess() == MountAccess::ReadWrite);
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("edited")).has_value());
			CHECK(ReadText(overlay, "project://Assets/Level1.scene") == "edited");
			CHECK(ReadText(overlay.GetLower(), "project://Assets/Level1.scene") == "lower level");
		}

		TEST_CASE("OverlayMount: ReleaseLower hands back the untouched lower mount" * doctest::skip(true))
		{
			OverlayMount overlay(MakeLower());
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("dry run")).has_value());

			const Scope<IMount> lower = overlay.ReleaseLower();
			REQUIRE(lower != nullptr);
			CHECK(ReadText(*lower, "project://Assets/Level1.scene") == "lower level");
		}

		TEST_CASE("OverlayMount: the case policy applies to the merged view" * doctest::skip(true))
		{
			OverlayMount overlay(MakeLower());
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Upper.scene"), AsBytes("upper")).has_value());

			CHECK(ErrorCodeOf(overlay.ReadFile(Path("project://assets/Level1.scene"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(overlay.ReadFile(Path("project://Assets/upper.scene"))) == ErrorCode::Validation);
		}

		TEST_CASE("OverlayMount: a new name that differs from an entry of either layer only in case is a validation error"
			* doctest::skip(true))
		{
			OverlayMount overlay(MakeLower());
			REQUIRE(overlay.WriteFileAtomic(Path("project://Assets/Upper.scene"), AsBytes("upper")).has_value());

			CHECK(ErrorCodeOf(overlay.WriteFileAtomic(Path("project://Assets/level1.scene"), AsBytes("x"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(overlay.WriteFileAtomic(Path("project://Assets/UPPER.scene"), AsBytes("x"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(overlay.CreateDirectories(Path("project://assets"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(overlay.Move(Path("project://Assets/Old.scene"), Path("project://Assets/LEVEL1.scene")))
				== ErrorCode::Validation);
			CHECK(ReadText(overlay, "project://Assets/Level1.scene") == "lower level");
			CHECK(overlay.GetChangedPaths() == std::vector<std::string>{ "Assets/Upper.scene" });

			// A case-only rename of a lower file happens in the overlay; the lower mount keeps its spelling.
			REQUIRE(overlay.Move(Path("project://Assets/Level1.scene"), Path("project://Assets/LEVEL1.scene")).has_value());
			CHECK(ReadText(overlay, "project://Assets/LEVEL1.scene") == "lower level");
			CHECK(ErrorCodeOf(overlay.ReadFile(Path("project://Assets/Level1.scene"))) == ErrorCode::Validation);
			CHECK(ReadText(overlay.GetLower(), "project://Assets/Level1.scene") == "lower level");
		}
	}

}
