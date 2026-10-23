# ADR 0003 — Distribution and install model

Status: proposed, recommended (2026-10-23)
Related: ADR 0001 (runtime), ADR 0002 (coordinates), master plan.

## Context

The desired user experience is a standalone engine + executables that the user points at their
existing KotOR install and runs. The engine must not ship, download, or redistribute game assets.
The original install should remain untouched: the new engine is not a patch and does not modify the
game it reads.

This is the model used by OpenMW, ScummVM, xoreos, and reone. It makes the user's own data the
source of truth and keeps the project's distribution to original engine/tool code plus permissively
licensed libraries.

## Decision

1. **Standalone runtime, not a patch.** Ship engine executables, launcher, tools, and third-party
   runtime libraries. Never patch or overwrite the original install.
2. **User-supplied data.** Require the user's game data set: `chitin.key`, `dialog.tlk`, `data/*.bif`,
   `modules/`, `rims/`, `lips/`, `TexturePacks/`, stream media, and optionally saves. The original
   `swkotor.exe` is **not required** and is never executed or read by the engine; this avoids a
   dependency on the original architecture and any DRM wrapper.
3. **No assets in the repository or installers.** No meshes, textures, audio, dialogue, fonts,
   extracted strings, or save data. No asset downloader. Caches are derived data, local, and
   deletable.
4. **Read-only install; external cache.** Detect the install root, read it without modification, and
   build a content-addressed cache in a user-data location (e.g. `%LOCALAPPDATA%/<project>/cache`),
   with a portable option. Cache entries carry source hashes and converter versions; invalid input
   invalidates the cache.
5. **Compatibility validation, not ownership verification.** Check for the required archives,
   version, language, and known hashes to give a clear diagnostic and choose a cache profile.
   Unknown hashes or Override/mod content produce warnings, not silent acceptance or deletion.
   There is no DRM, license check, or proof-of-purchase mechanism. Presence of data is not proof of
   ownership; the project relies on the user's licence and applicable law.
6. **First-run UX.** Auto-detect common Steam/GOG paths; let the user browse to a folder; show a
   validation report (missing files, unsupported version/language, mods detected); build/refresh
   the cache with progress and a cancel path; then launch. Fail closed with actionable messages.
7. **Support matrix.** KotOR 1.0.3.0 English first (Steam/GOG data layouts); more languages and
   versions later. KotOR 2 is out of scope for the initial program. Modded installs are supported
   through the read-only VFS and Override precedence; caches must be per-install/profile.
8. **Licensing and branding.** Engine code and dependencies only; neutral project name and
   non-affiliation notices; no Lucasfilm/BioWare/EA/Star Wars branding or trade dress. Public
   release requires IP/EULA counsel; a storefront listing is not assumed.

## Consequences

- Onboarding and diagnostics are part of the engine deliverables, not an afterthought.
- Support must handle partial, modded, localized, and multiple installs; caches and logs are keyed
  by install identity.
- The engine cannot function without user data, which is a practical/legal posture but **not** a
  technical ownership guarantee. We do not implement DRM or ownership checks.
- The installer/launcher never needs administrator rights to modify the game folder.
- Save handling must choose whether to read/write the original saves folder or a separate profile;
  either way, writes are explicit and reversible.

## Future possibility — optional Steam ownership verification (not implemented)

A future distribution variant could require the user to sign in to Steam and verify entitlement
(e.g. Steamworks user authentication plus an app-ownership check for KotOR's AppID). This is
recorded as a possibility only and is **out of scope for the current engine**:

- It requires a Steamworks partner/dev account, app configuration, SDK integration, and legal/privacy
  terms; it is distribution policy, not engine technology.
- It would exclude GOG/offline/other-store users unless a parallel path exists. Any future design
  must explicitly decide whether that is acceptable.
- It must live in a separate launcher/policy layer, never in the VFS, simulation, save, or renderer
  code, so the engine remains usable with a user-owned data install without a login.
- Current validation remains compatibility-only (files/version/language/hashes). No DRM, login, or
  ownership check is implemented now.

## Open questions

1. Default cache location and portable-mode layout.
2. Whether to support a custom profile/saves directory by default or only on request.
3. Install validation policy for unknown hashes and heavily modded installs (warn vs block).
4. Public-release legal review and naming.
5. Future entitlement-check strategy (Steam or otherwise) and whether it is ever adopted; currently
   out of scope.
