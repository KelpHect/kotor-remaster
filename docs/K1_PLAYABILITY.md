# K1 playability closeout

Status: **implemented and gated for the playable surface; bounded residuals
remain.** This work closes the playability items that the Sprint 15-18 roadmap
did not cover, except for the evidence-bounded items listed at the end. It is
not a claim that a full quest-completion playthrough has already been played.

## What was added

### Character generation

- `engine/src/game/Chargen.*` is a deterministic state machine for the three
  base classes: 30-point attributes, class/cross-class skills, a level-1 feat,
  `portraits.2da` selection and a name.
- `--chargen` opens the interactive flow; New Game in the shipped main menu
  also starts it.
- `--chargen-test` headlessly builds a Scout, spends attributes and skill
  points, selects a feat and portrait, applies the sheet to the live session,
  saves, reloads and verifies every field.
- `GameSession::applyCharacter` and `setPlayerAppearance` feed the same sheet
  into the player avatar.

### Progression

- Level-up awards class skill points, feat points and an attribute point every
  fourth level.
- `spendSkillPoint` and `spendAttributePoint` are exposed in the character
  panel.
- Engine saves persist per-skill ranks and attribute points; original
  `SAVEGAME.sav` writes the real `SkillList` ranks instead of eight zeros.

### Player avatar and camera

- `kotor::spawnPlayerAvatar` builds the player's body from `appearance.2da`
  and attaches a `heads.2da` head.
- `--third-person` renders the avatar and uses a collision-aware orbit camera;
  tight spaces fall back to the first-person camera and hide the avatar rather
  than clipping through it.
- `--player-appearance`, `--player-name`, `--no-movies` and
  `--camera-noclip` support deterministic captures and inspection.
- MDL skinning now uses the real bone-slot map, XYZW rotation keys and the
  mesh-space bind transform, so the player and NPCs animate without the
  previous exploded vertices. The avatar plays `run`/`pause1` and follows the
  collision-validated camera.
- Party companions are spawned from their UTC templates (with separate heads
  where the body model has no `head_g`), follow the leader, play locomotion,
  and survive save/reload. `--avatar-test` asserts the player and Mission
  meshes, locomotion and companion follow.

### Dialogue faces

- The active speaker's body mesh receives the `FacialRig` viseme rotations
  through `Mdl::posedTrianglesRigged`, so jaw/mouth/brow bones animate during
  voiced dialogue.
- `kotor_media` asserts `facial_scene_dialogue` and `facial_scene_pose`.

### Playable minigames

- Pazaak, swoop and turret each have an interactive panel.
- `P`, `O` and `T` open them during play; `--start-minigame
  pazaak|swoop|turret` opens one deterministically for captures.
- Pazaak drives the existing deck/side/set rules; swoop reads W/Shift/S;
  turret fires at live approaching targets.
- The `SWMG_*` script bridge now creates a real space-minigame session:
  `SWMG_GetEnemyCount/GetEnemy` start the turret state, `SWMG_GetPlayerSpeed`
  starts the swoop state, and player/enemy/HP/event queries return live state
  instead of scalar scratch cells. The shipped minigame scripts therefore
  drive the same state the panels use.
- Story triggers still depend on the shipped scripts reaching their SWMG calls;
  the engine does not force a particular module into a minigame by name.

### Shipped character-generation screens

- `--chargen-gui` runs `classsel` -> `portcust` -> `abchrgen` -> `skchrgen` ->
  `ftchrgen` -> `name` through `GuiSystem`; `BTN_ACCEPT`, `BTN_BACK`, +/-,
  portrait arrows, `BTN_SELECT`, `END_BTN` and the name box are routed into
  `Chargen`.
- `--chargen-gui-test` exercises the same tag sequence headlessly and asserts
  class, portrait, attribute, skill, feat, name and final applied sheet.
- The 3D model preview and portrait textures are not rendered yet; the
  functional controls and layout are shipped-screen based.

### Campaign continuity gate

- `--campaign-test` runs the deterministic Endar Spire playthrough, then loads
  every retail save fixture, advances each 600 frames and asserts module
  identity, clock, player survival, entity content, scene warnings and journal
  presence. It then loads Dantooine, Kashyyyk, Manaan and Korriban anchors.

## Evidence

- `engine/tests/fixtures/retail_traces.json`
- `kotor_chargen` (CTest)
- `kotor_chargen_gui` (CTest)
- `kotor_campaign` (CTest)
- `kotor_avatar` (CTest)
- `kotor_media` facial scene assertions
- `kotor_minigame` rules gate
- `docs/evidence/s14/ctest.log`

## Gate

- CTest: **55/55**, 1286.79 s (120-trace differential 216.59 s).
- 117-module sweep: **117/117**, 0 failures/warnings.
- VM corpus: **8,666/8,666**, 0 mismatches, 303,616 ACTION calls.
- GL/Vulkan parity: **0.0671** mean channel diff, PASS.
- Python tools: **143 tests**, 7 skips.
- Install sentinel: **15,380 files**, 0 session changes.

## Remaining boundaries

- The minigame panels are playable, but the shipped `SWMG_*` script bridge still
  stores scalar cells; story-triggered swoop/turret modules are not auto-wired.
- Companion tags/save ids still use the legacy table for some names; the visual
  party path is implemented, but full retail `PT_MEMBER_ID` parity needs the
  save-id experiment.
- The campaign gate is a multi-act continuity smoke test, not a quest-completion
  proof. A real end-to-end run still needs player input or a quest bot.
- External `.rsv` shadow files now parse ERF/MOD/SAV and RIM containers; a
  future container form still needs a fixture.
- `videoeffects.2da` post-processing (`EnableVideoEffect`/`DisableVideoEffect`,
  DLG `CamVidEffect`) runs on GL and Vulkan. Per retail, only a static dialogue
  camera applies a row, -1 leaves the state unchanged, other cameras and the end
  of the dialogue disable it, and undefined rows are rejected and counted. The
  scan-noise texture is a deterministic procedural stand-in; the retail pass
  order and per-frame noise regeneration are unverified.
- Reverb is a parameter-faithful FDN with early reflections (not bit-exact
  Miles/EAX): per-room parameters come from the ARE `EnvAudio` of the listener's
  room, sends are on positional voices only at 0.6, music and movies stay dry.
  Not implemented: Creative DSP texture, modulation (UNDERWATER), air absorption,
  LF decay, room rolloff, DecayHFRatio > 1, the AmbientScale volume ramp; ids >= 24
  map to GENERIC (inferred) and the retail level source beyond the constructor
  was not fully traced.
- The shipped chargen screens are functional, but `3D_MODEL*`/`MODEL_LBL` and
  portrait textures are still placeholders.
