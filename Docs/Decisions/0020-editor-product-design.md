# Editor product design

Status: accepted, 2026-10-10. The product owner requested a dedicated editor redesign after inspecting the M10 interface, explicitly permitting replacement of the existing UI. This is a refinement of M10 before M14, not a new engine subsystem.

## Direction and scope

The editor uses a restrained charcoal palette, readable Inter typography (the already pinned and licensed resource), consistent spacing, subdued borders and one blue accent for focus and selection. Primary workflows should be clear in populated, empty, error and play states. No new third-party assets or libraries are required.

The default workspace distinguishes the menu, main toolbar, scene tools, content area and status. Panel display names are human-readable; protocol names and existing saved window and dock-tab identities remain stable. The pinned Dear ImGui version hashes a title's suffix after `###` identically to the previous raw title. Inspector fields use aligned labels and controls, readable references and progressive disclosure for internal and advanced values. Common transform editing exposes position, Euler rotation and scale. Raw quaternion and derived transform values remain accessible in an advanced section. Asset browsing and scene opening support browsing instead of requiring users to type paths. Existing mutation, undo, read-only and automation rules continue to apply.

Shared presentation helpers live under `Editor/Ui` and have no mutable global state. The Editor owns fonts and theme; the generic engine ImGui backend and Runtime keep their existing appearance. UI helpers and panels are exercised by the existing headless ImGui tests. There is no contract-stub commit for this refinement of implemented interfaces; all commits use the strict gate.

`FontImporter::ValidateSource` exposes the existing sfnt structural check without atlas baking so the editor can validate its pinned font before ImGui reads it. This additive API keeps the parser private to AssetPipeline and avoids a second validation implementation. Font bytes are copied into ImGui-owned allocation and survive the source mount; the font tests cover missing, truncated and malformed resources, source lifetime and display scaling.

Display scaling is the monitor content scale divided by the framebuffer-to-window ratio. Windows pixel-coordinate windows receive the monitor scale in the UI; Retina logical-coordinate windows already receive it in the framebuffer. Rebuilding the style from its base metrics prevents accumulation when moving between displays. While the Game view owns input, the viewport host suspends ImGui keyboard navigation so gameplay arrows cannot move UI focus and release held game keys. Losing focus restores navigation and releases game input through the existing host path.

## Implementation ownership

- Lead: shared style and font lifecycle, viewport panels, shared integration files, documentation, visual evaluation, golden candidates and final gates.
- Workspace stream: EditorLayer and SceneHierarchyPanel; scene browsing; associated focused tests.
- Inspector stream: InspectorPanel and ReflectedDrawers; associated focused tests.
- Assets stream: ContentBrowserPanel, ProjectLauncher and FolderPicker; associated focused tests.
- Supporting panels stream: Console, Diagnostics, ProjectSettings, Stats, Automation and UndoHistory; associated focused tests.

## Acceptance

- Review actual screenshots at 1280x720 and 1600x900, plus a larger/scaled presentation; inspect selected entities, populated assets, launcher and supporting panels.
- Main controls and property labels fit at the default layout; narrower panels wrap or scroll rather than hiding required actions.
- Scene/project paths and asset identifiers are presented as readable names with full details available on demand.
- Exercise selection, inspector editing and undo, asset browsing, scene opening and play/stop through the production paths. Preserve the existing automation coverage.
- Theme scaling is absolute, not cumulative; saved layouts survive the visible title changes.
- Review new golden images deliberately. Passing image comparisons alone does not establish design quality.
- Strict PreCommit and full local CI pass before integration. Remote platform verification remains non-blocking under ADR 0011.

## Integration review

The lead and implementation streams reviewed the production controls, state transitions, drawing and tests against `ReviewChecklist.md`. The following findings were resolved during integration:

1. Font validation crossed a private module boundary. The editor now calls the public, pure `FontImporter::ValidateSource` entry point.
2. Monitor scale could be applied twice on Retina. The editor normalizes it by the framebuffer ratio and tests both coordinate conventions.
3. ImGui keyboard navigation could steal held gameplay arrows. The viewport host suspends navigation while Game owns input; a production GPU test holds an arrow for thirty frames and then checks release on blur.
4. World/Local and Pause/Resume changed widget identity with their labels. Stable suffixes retain keyboard focus while the visible state changes.
5. A pending hierarchy rename could address a replacement runtime entity with the same UUID. Rename state now carries the play-session serial and scene generation.
6. Inspector name buffers and pending field gestures could survive scene replacement. Owner identities include the project binding and scene/session epoch; replacement cancels pending commits even before another UI frame. Project paths use the existing UTF-8 conversion.
7. Scene browsing only searched `Assets`. It now discovers scene files throughout `project://`, including root scenes and other project folders.
8. A stale recent-project path prevented browsing. The picker falls back through accessible parents, then to the default location.
9. Authored file and folder names containing hash markers could collide or disappear in ImGui labels. Full-byte ID scopes and literal labels cover asset tiles, trees, breadcrumbs, move destinations, scene lists, project recents and file-picker entries.
10. Authored diagnostic and automation text had the same identity problem. Independent Details controls and source navigation are covered with matching hash suffixes.
11. History inferred saved state only from its undo position. It now reads the editor's scene-dirty state, including repaired scenes.
12. History disappeared when no scene was open even though project-setting commands remained undoable. Project history remains accessible without a scene.
13. File-picker identities based on instance addresses could create stale saved window records. Transient pickers no longer persist layout settings.
14. Several new include groups were out of order. The changed files follow the repository's include ordering and formatting rules.
15. Validation hints exposed protocol syntax in the primary Problems flow. Complete technical hints remain accessible in Details alongside diagnostic codes and IDs.
16. Clang's compiler-based lint found an unused test lambda that MSVC accepted. The unused local was removed without changing the test's actions or assertions.

Visual review uses actual production controls and rendering: selected objects at 1280x720 and 1600x900, 2560x1440 at 2x, the launcher, and compact Settings/Console/Problems/Performance panels. The existing `EditorDefaultLayout` golden is intentionally updated for the font, spacing and workspace; engine rendering goldens retain their existing expectations. CPU interaction tests cover edits, undo, read-only states, filtering, file browsing and replacement lifetimes. The existing automation methods remain the mutation path, so this refinement adds no protocol entries or catalogue changes. No contract stubs or permanent test skips are introduced.

Checklist disposition: sections 0-5 and 8-11 apply to the changed code, tests and documentation. Sections 6-7 retain deterministic simulation and threading behavior; the UI remains on the main thread. Sections 12-13 introduce no vendor, build-configuration, download or protocol-security changes. Local build/test results and the final review verdict are recorded in the commit trailer; Linux and macOS remain subject to the non-blocking remote verification policy.
