# Recon 52 - GUI screens and control trees

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Inventory

`gui.bif` holds 84 GUI resources (type 2047); `patch.erf` holds 7, of which
`mipc212x10` and `tooltip12x10` are patch-only, giving **86 unique screens**
and **91 resource occurrences**. The shipped set parses to **2,383 controls**
across 8 control types (0 control, 2 panel, 4 image, 5 label, 6 button,
7 checkbox, 8 slider, 9 scrollbar, 10 progress, 11 listbox) including 81
nested listbox scrollbars; no unknown type occurs. The largest screen has
127 controls.

## Format

A `.gui` file is GFF V3.2 with magic `GUI `:

- root `CONTROLS` list, `CONTROLTYPE`, `TAG`, `EXTENT`, `ALPHA`, `COLOR`;
- each control struct repeats `CONTROLTYPE`, `TAG`, `ID`, `EXTENT`;
- optional `CONTROLS` child list (leaf controls may omit it);
- listboxes carry a `SCROLLBAR` struct (type 9) as a nested control;
- `BORDER` / `HILIGHT` structs with `FILL`, `EDGE`, `CORNER` resrefs;
- `TEXT` struct with `TEXT` and `FONT`;
- sliders/progress/scrollbars carry `MAXVALUE`/`CURVALUE`/`STARTFROMLEFT`;
- control extents are in 640x480 GUI space.

`parseGui` recursively parses the control tree, resolves extents/colors/text/
border resrefs, assigns a stable hierarchy key (`LB_DESC/SCROLLBAR`) and counts
controls.

## Interaction/state

`GuiSystem` (engine-side, since shipped `.gui` files carry no script fields):

- hit-tests visible controls in draw order and tracks hover/pressed state;
- button/checkbox/listbox/image actions fire a tag action; `BTN_EXIT` sets a
  quit request;
- sliders, progress bars and scrollbars clamp values to `MAXVALUE`;
- listboxes track selection;
- `open`/`push`/`pop`/`close` implement screen transitions with a stack;
- `GameSession::applyGuiAction` maps `LBL_ICON1..4` to the character,
  inventory, party and journal panels.

`--gui-screen <resref>` renders the interactive tree and `--gui-action <tag>`
activates one control for deterministic screenshots. `mainmenu` now has 15
controls (the module listbox carries a scrollbar) and renders at 960x540
(`docs/evidence/s13/gui-state.png`).

## Gate

`--gui-test` asserts:

- 86 unique screens (`screens=86`);
- 2,383 controls (`controls=2383`);
- 8 known control types (`control_types=1`) with zero unknown types;
- `mainmenu` is `TGuiPanel` with 15 controls (`mainmenu=1`);
- maximum control count 127;
- hit-testing, pointer down/up, button action and action log;
- visibility, exit/quit, push/pop transitions;
- slider/progress/scrollbar clamping, listbox selection, checkbox toggling;
- HUD icon panel mapping and toggling.

## Evidence

- `docs/evidence/s13/gui.log`
- `docs/evidence/s13/gui-state.png`, `gui-mainmenu.png`
- `engine/src/kotor/Gui.*`, `engine/src/game/GuiSystem.*`
- `engine/src/app/Application.cpp` (`--gui-screen`/`--gui-action` overlay)

## Paired frame findings (UI pass)

Addresses are swkotor.exe 1.0.3.0. Labels say how each fact is known.

- Options help: table per screen from the constructors 006e69a0, 006e6710, 006e16e0, 006e0460 and siblings (retail code). Hover moves the focus; a control with no event 0 handler keeps the text.
- Feedback options: nine rows at 006e1300, help text per row, bit in the party's feedback word (retail code). Consumers: 0x1 subtitles (00631d80, 006a70a0), 0x2 hide-unequippable (006b9470), 0x8 minimap, 0x10 floating numbers (006027c0, 0068b7c0), 0x20 status summary pop-up against icons (00602eb0), 0x100 small font, whose only reader found centres two map buttons (00693650), 0x200 HUD, tooltips (00624af0).
- Tabs: `top.gui` merged into the eight in-game panels, `LBLH_x` lit for the page or hover (00627980, 00624c00, 00624c30, 00624cf0). Keys from keymap.2da rows 209..216 (00621210).
- Inventory: six lists through 006b2c90, names at 00756444, title and button at 006b2a80 (retail code). Worn items first with their mark (paired frame). New Items lists what giveItem added since the inventory last closed (clearing on close is inferred; retail keeps the bit in the save, ours does not).
- Messages: opens on the dialog log (paired frame); title and button at 00624df0, 00624f70.
- Map: pins and notes 00692c70, 006929b0, 006927b0, 006927c0. Explored cells 00578c60, 005792d0, 004b4e80; unseen cells drawn with `blackdot` at four cells' size on both minimap (00688100) and map (006943d0); pins filtered by 00579210. Art tiled per 00687e40; party circles lbl_mapcircle (00693f60), tint inferred. Refusal is the OK box of 00692bc0. Explored cells are not in our saves (retail 005061d0, 00505da0).
- Galaxy map: 00694b40 first available and selectable row; models in 3D_PlanetDisplay and 3D_PlanetModel. `LB_MODULES` and `BTN_WARP` are bound only by the main menu (0067ace0). Live planet rows 12..16 name art that does not ship.
- HUD: `mipc` file per exact retail size (0068c4e2), 1:1 scale there. Other sizes are an engine choice: the compact file scaled by max(1, height/720). Panels scale in ours; retail draws them unscaled inside a surround at larger sizes.
- Name entry: characters per 004184f0. Random name 006f9bf0 -> 007118c0 -> 00711390 from humanm/humanf/humanl, filtered by namefilter.2da.
- Feat screen: 006f3d60 builds Add Feat and Remove Feat; labels are bound. Rows of three icons per 006ce570, 006cda80, on level-up and the abilities feat tab (006abce0, 006ce0f0).
- Character sheet: the body stands in `charrec_light` (006b0e40, 006afda0, 006100f0). Clip `evil`, `good` or `align` + alignment/5 + 1 drives the emitters and the camera hook's height; stance evil, neutral or good on the body. Field of view 0x41b5ced9. MAINCG (006e8ef0, 006eb420) uses the same scene; ours has no MAINCG hub.
- Galaxy map: `galaxy.mdl` is emitters alone; planets zoom per camerahook in `zoomin`, then `rotate` (006933a0).
