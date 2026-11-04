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
