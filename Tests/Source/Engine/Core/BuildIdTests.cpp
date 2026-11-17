#include "TestsPCH.h"
#include "Engine/Core/BuildId.h"

#include <string>
#include <string_view>

namespace Engine {

	TEST_SUITE("Core")
	{
		// Docs/Decisions/0030-m19-contract.md decision 13 (B-BVER): the build id is semantic-versioning build metadata, the
		// version alone without a commit, "+<commit>" for a clean tree and "+<commit>.dirty" for a dirty one, and parses back
		// strictly; this binary's own id carries EngineVersionString.
		TEST_CASE("BuildId: the build id formats as semantic-versioning build metadata and parses back")
		{
			const BuildId clean{ .Version = "0.2.0", .Commit = "3f2a9c1d4e5b", .Dirty = false };
			const BuildId dirty{ .Version = "0.2.0", .Commit = "3f2a9c1d4e5b", .Dirty = true };
			const BuildId unstamped{ .Version = "0.2.0", .Commit = {}, .Dirty = false };
			CHECK(FormatBuildId(clean) == "0.2.0+3f2a9c1d4e5b");
			CHECK(FormatBuildId(dirty) == "0.2.0+3f2a9c1d4e5b.dirty");
			CHECK(FormatBuildId(unstamped) == "0.2.0");
			for (const BuildId& id : { clean, dirty, unstamped })
			{
				const Result<BuildId> parsed = ParseBuildId(FormatBuildId(id));
				REQUIRE_MESSAGE(parsed.has_value(), parsed.error().ToString());
				CHECK(*parsed == id);
			}

			for (const std::string_view text : { "", "0.2", "0.2.0.1", "00.2.0", "0.2.0-rc1", "+3f2a9c1d4e5b", "0.2.0+", "0.2.0+3F2A9C1D4E5B",
					 "0.2.0+3f2a9c1", "0.2.0+3f2a9c1d4e5b.clean", "0.2.0+3f2a9c1d4e5b.dirty.x", "0.2.0.dirty", "v0.2.0" })
			{
				CAPTURE(text);
				const Result<BuildId> refused = ParseBuildId(text);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}

			const BuildId& own = GetBuildId();
			CHECK(own.Version == EngineVersionString);
			CHECK((own.Commit.empty() || own.Commit.size() == 12));
			CHECK((!own.Dirty || !own.Commit.empty()));
			const Result<BuildId> ownParsed = ParseBuildId(FormatBuildId(own));
			REQUIRE(ownParsed.has_value());
			CHECK(*ownParsed == own);
		}

		// The reservation ledgers own the version (Docs/Decisions/0028-backlog-program.md): the last lander of each phase
		// bumps its minor version, 0.3.0 at M23's landing, phase 2's last (Docs/Decisions/0034-m23-contract.md decision 17), 0.4.0
		// at M26's, phase 3's last (Docs/Decisions/0037-m26-contract.md decision 19), 0.5.0 at M27's, phase 4's last
		// (Docs/Decisions/0038-m27-rolling-ball.md decision 13), 0.6.0 at phase 5's second batch landing, M30's and M33's
		// (Docs/Decisions/0042-world-tree-engine-program.md, the ledger's EngineVersionString row), and 0.7.0 at phase 6's
		// landing of M36 to M40 (Docs/Decisions/0052-world-tree-phase-6.md, the ledger's EngineVersionString row). A new
		// recording's replay header records it too (ADR 0038, friction F10: ReplayMethodsTests), so the bump regenerated
		// FeatureTest's replays, while a committed demo's replays keep the version they were recorded with.
		TEST_CASE("BuildId: the engine version is phase 6's")
		{
			CHECK(EngineVersionString == "0.7.0");
			CHECK(GetBuildId().Version == "0.7.0");
		}
	}

}
