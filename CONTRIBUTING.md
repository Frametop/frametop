# Contributing

Full Keyboard for Steam Frame welcomes bug reports, review, and focused fixes. The app serves the Frame dashboard and local applications. Optional native keyboard replacement is experimental and must preserve native fallback when the app is unavailable. Typing into streamed VR apps is outside the current scope.

## Report a bug

Include the app version, SteamOS/runtime version, controller model, affected app, desktop versus floating-window mode, selected language/layout, reproduction steps, and expected/actual result. Include a screenshot or a short log only if it helps. Remove passwords, private text, account details, and clipboard contents.

For Japanese input, include a harmless example reading, the expected conversion, and the selected Romaji/Kana/JIS mode. Feedback from fluent Japanese users is especially useful.

## Dependencies and third-party code

Do not bundle whole third-party projects inside this repository, including through submodules or source snapshots. Keep external projects separate and use installed libraries, tools, or services through their documented interfaces. Normal builds and installation must not implicitly download external projects or large runtime assets.

Occasional reference files, headers, protocol definitions, and small helpers are fine when they serve a clear purpose. Record their upstream source, exact revision, local modifications, and applicable licenses and attribution. Check embedded components and transitive dependencies; a project's root license may not cover every file. Preserve the required notices in source and release packages.

Document external dependencies and any separate setup they require. Keep optional dependencies optional during build, installation, and startup. Do not enable autostart, start external services, or change system configuration implicitly.

## Optional features and capabilities

Optional features must leave ordinary keyboard operation usable when a dependency is absent, loading, incompatible, or disconnected. Verify readiness and capabilities through the dependency's interface rather than assuming that an installed file or running process is sufficient. Keep integration-specific code behind a defined interface so the UI and profile data do not depend on a particular implementation.

External interfaces must have a documented version and capability contract. Report capabilities for the active configuration, including supported languages, formats, and operations where relevant. Define compatibility and locale matching explicitly. Validate requests against current capabilities, handle capability changes, and reject unsupported operations without silently changing the requested behavior.

Express capability-dependent controls declaratively in profile data. Account for the selected layout, language, and input backend. Use a small validated set of conditions; do not execute scripts or arbitrary expressions from profile files. Hidden controls must be absent from both rendering and hit testing. Keep cancellation controls accessible during active operations, and apply visibility changes without stranding held input or moving another action under a pressed pointer.

These are contribution requirements, not a claim that every interface or profile condition already exists. Include parsing, validation, documentation, and tests when introducing a new contract or schema field.

## Version ownership

Only the project owner, TaiKeid, may bump the project version. Contributors must not change the version in `CMakeLists.txt` or update release-version references as part of their contributions. The owner handles these changes during release preparation.

## Change the code

Read [building](docs/building.md), [architecture](docs/architecture.md), and [configuration](docs/configuration.md). Keep renderer, key state, backend, and profile data separate. Use `.clang-format` for project C++; preserve vendored code's style and attribution.

Run the core test suites for behavior changes. Test input with fake sinks or a dedicated receiver, never an arbitrary focused app. Cover lost releases, tracking loss, disconnects, hiding, and shutdown when changing interaction ownership. Add comments where state lifetime, coordinate transforms, or event ordering are not obvious.

Submit focused PRs and describe the behavior, validation, and remaining limits. Separate unrelated fixes, features, UI changes, and refactoring when they can be reviewed independently. Do not include sysroots, builds, device logs, personal paths, credentials, or user settings. Do not mix publication changes into unrelated PRs.

Every owned resource and subprocess needs a cleanup path for cancellation, hide, disconnect, and shutdown. Invalidate late asynchronous results and guard delayed input against destination changes. Do not terminate shared external services. Bound buffers and waits, and keep blocking work off the VR loop. Deliver text through the existing Unicode transport and retain unsent text when delivery fails. Do not log or persist user input.

Verify installation and packaging from a clean checkout without relying on untracked files or private local setup. For affected paths, cover missing dependencies, capability changes, unsupported configurations, resource cleanup, stale results, visibility and hit testing, non-US text delivery, and failed commits. State whether validation ran on the host, an ARM64 cross-build, the Frame, or in the headset. An ARM64 build alone does not verify runtime behavior on the device.

## Review priorities

Input cancellation, focus routing, two-controller ownership, placement persistence, resource cleanup, parsing of custom profiles, and runtime compatibility deserve close review. Tests and successful builds do not replace in-headset validation; say which one was performed.
