# ADR 0001 — Runtime, RHI, and graphics strategy

Status: accepted (2026-10-23; user selected the custom C++23 + SDL3 + Vulkan 1.4 core)
Related: master plan, engine lane, renderer lane.

## Context

The goal is a 1:1-compatible reimplementation of KotOR 1 that loads the user's original data,
runs the original scripts and rules, and can later be enhanced (higher resolution, better
shadows, post-processing, higher frame rates) without forking the game logic.

The original renderer is OpenGL 1.x-era fixed function: lightmapped static geometry,
per-vertex lighting, stencil-buffer shadow volumes, alpha test/blend, fog, billboard/CPU
particles, and 2D GUI quads. The visual identity is mostly a function of the 2003 assets and
that pipeline, not of the graphics API. KotOR's frame cost is dominated by CPU-side scene
submission, animation, and area/script logic, not by modern GPU capability.

Decompilation and RE produce behavioral specifications, formats, and an independently written
runtime. They do not by themselves "compile to anything"; portability comes from keeping the
runtime engine-agnostic and writing a renderer abstraction.

## Decision

1. **Engine runtime:** an original, data-driven C++23 runtime (not a licensed engine as the
   core). SDL3 for platform, windowing, input, and audio device. A fixed-step simulation with
   render interpolation. All Odyssey formats, scripting, save data, and game rules live in
   first-party code.
2. **Render hardware interface (RHI):** a thin, explicit RHI owned by the project.
   - **Vulkan 1.4 is the primary API target** (latest stable core; dynamic rendering,
     synchronization2, timeline semaphores, VMA, descriptor indexing where useful). Use
     core 1.4 features where they remove extensions or simplify the renderer, but keep a
     **Vulkan 1.3 compatibility floor** with runtime feature detection for older drivers and
     MoltenVK. Vulkan gives explicit control, predictable frame pacing, and portability to
     Windows/Linux/Steam Deck; MoltenVK covers macOS if needed.
   - **OpenGL 4.5 is the reference/fallback backend.** It is faster to iterate on, easier to
     debug, and the closest mental model to the original pipeline. It is the oracle backend for
     1:1 visual comparisons.
   - D3D12 and Metal can be added behind the same RHI later; no game code changes.
3. **Two render paths over one simulation:**
   - **Classic**: emulates the 2003 fixed-function stages and original draw parameters. This is
     the default for 1:1 verification and for users who want the original look.
   - **Remaster** (optional, later): modern enhancements fed by the same scene data — higher
     internal resolution, MSAA/SSAA, improved shadow maps as an alternative to stencil volumes,
     SSAO, bloom/tonemapping, high-res texture pack overrides, optional PBR approximations,
     widescreen/HFR, better LOD and culling. It must be switchable at runtime and must never
     affect simulation state or save data.
4. **No GPU API in gameplay code.** Game systems talk to a renderer/scene facade; only the RHI
   backend sees Vulkan/GL objects. The same rule applies to audio and input.
5. **Shader toolchain:** SPIR-V as the interchange format; GLSL via glslang or Slang; cross-compile
   to GLSL for the reference backend. Shaders are part of the project, never extracted from the
   original.
6. **Validation:** the Classic backend is the visual oracle. Automated captures (fixed camera,
   fixed seed, fixed tick) are compared against documented original behavior and, where
   appropriate, licensed reference captures. The Remaster path is validated separately for
   "no gameplay divergence".

## Why not Unreal Engine 5 as the core?

UE5 buys animation, editor, console certification, and an asset pipeline, but its renderer
(Nanite/Lumen/TAA) actively fights a 2003 fixed-function look. A 1:1 path would require custom
RHI/shader work inside UE anyway, while cooked assets and the engine object model complicate
loose-file modding and original-save compatibility. Royalty and iteration cost are additional
constraints. A custom runtime is more work but has fewer semantic mismatches.

## Why not Godot as the core?

Godot 4.7 is a credible fallback (MIT, Vulkan via RenderingDevice, fast iteration). It still
requires custom VFS, all Odyssey parsers, the NWScript VM, walkmesh/pathfinding, save
compatibility, and a custom compatibility renderer. GDExtension ABI churn and the scene/resource
model are real costs. It is the recommended off-the-shelf option if editor velocity becomes
more important than maximum fidelity control.

## Why Vulkan 1.4 now?

- It is the current stable core and avoids building new code against a version that is already
  a compatibility target. Core 1.4 folds in maintenance and synchronization fixes that would
  otherwise be extensions.
- One modern API across Windows/Linux (and macOS through MoltenVK), with explicit control that
  will not box in later enhancements.
- Better long-term fit for high-refresh, variable-rate, multi-threaded command recording, and
  GPU-driven batching if the remaster path needs it.
- The RHI boundary keeps the 1:1 backends simple; Vulkan's complexity is contained. Feature
  detection plus the 1.3 floor means a 1.4-only driver is not a hard requirement on day one.

The cost is real: synchronization, memory, pipeline state, and debugging are harder than GL.
That is why the GL reference backend exists and why the first vertical slice ships on both.

## Consequences

- We must build tooling: shader compiler pipeline, pipeline cache, resource lifetime rules,
  GPU validation in CI, and a frame capture harness.
- We must keep a strict separation between simulation and presentation to make Classic/Remaster
  and backend swaps possible.
- Ports (Steam Deck, Linux, macOS) come from SDL3 + RHI, not from rewriting the game.
- Performance work targets CPU-side systems (jobs, culling, batching, async I/O, load-time
  conversion) because that is where KotOR's bottlenecks are; Vulkan alone will not fix them.
- "More modern graphics" is a content/pipeline problem as much as an API problem: the original
  meshes, textures, and lighting data cap what can be shown. Remaster features must be explicitly
  designed and validated, not assumed.
