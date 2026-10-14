"""The portability stage's checks of generated build settings for the code paths only other platforms run (Docs/
Architecture.md §14.1, §15.8, §16; Docs/Decisions/0024-m15-contract.md decision 18). Stream D implements this module; the
lead's CI.py portability stage calls it after its existing generated-settings check, on the directories it generated
into bin-int/Portability/.
"""

from __future__ import annotations

from pathlib import Path


def check_runtime_link_settings(generated: dict[tuple[str, str], Path]) -> list[str]:
    """The problems of the Runtime's generated link settings, one line each ("<system>-<action> <config>: <problem>"),
    over `generated` ({(system, action): the directory premake wrote}), for every configuration (Debug, Release, Dist):
      - linux gmake (Runtime.make ALL_LDFLAGS) and linux ninja (Runtime.ninja ldflags_Runtime_<Config>): the link line
        holds -Wl,-rpath,'$$ORIGIN' (make and ninja turn $$ into $, the quotes keep the shell from expanding it) and
        -Wl,-Map= naming bin/<Config>-linux-x86_64/Runtime/Runtime.map;
      - macosx xcode4 (Runtime.xcodeproj/project.pbxproj): OTHER_LDFLAGS holds -Wl,-rpath,@executable_path/../Frameworks
        and -Wl,-map,; MACOSX_DEPLOYMENT_TARGET is 14.0; the product type is com.apple.product-type.tool (the exporter
        copies a plain executable and assembles the bundle itself);
      - windows vs2026 (Runtime.vcxproj): /MAP in the Link AdditionalOptions of every configuration (premake writes the
        linkoptions of Runtime/premake5.lua there; there is no GenerateMapFile element), SubSystem Windows in Dist and
        Console otherwise.
    A target missing from `generated` is a problem."""
    raise NotImplementedError("contract stub: portability (ADR 0024 decision 18, stream D)")
