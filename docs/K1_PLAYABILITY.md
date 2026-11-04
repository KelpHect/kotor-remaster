# K1 playability closeout

Status: **implemented and gated** (post-Sprint 14). This work closes the
playability items that the Sprint 15-18 roadmap did not cover. It is not a
claim that a full quest-completion playthrough has already been played.

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

### Campaign continuity gate

- `--campaign-test` runs the deterministic Endar Spire playthrough, then loads
  every retail save fixture, advances each 600 frames and asserts module
  identity, clock, player survival, entity content, scene warnings and journal
  presence. It then loads Dantooine, Kashyyyk, Manaan and Korriban anchors.

## Evidence

- `engine/tests/fixtures/retail_traces.json`
- `kotor_chargen` (CTest)
- `kotor_campaign` (CTest)
- `kotor_media` facial scene assertions
- `kotor_minigame` rules gate
- `docs/evidence/s14/ctest.log`

## Remaining boundaries

- The minigame panels are playable, but the shipped `SWMG_*` script bridge still
  stores scalar cells; story-triggered swoop/turret modules are not auto-wired.
- Companion tags/save ids still use the legacy table for some names; the visual
  party path is implemented, but full retail `PT_MEMBER_ID` parity needs the
  save-id experiment.
- The campaign gate is a multi-act continuity smoke test, not a quest-completion
  proof. A real end-to-end run still needs player input or a quest bot.
- The character-generation UI is an ImGui flow, not the shipped `classsel` /
  `name` screens.
