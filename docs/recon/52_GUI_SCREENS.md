# Recon 52 - GUI screens and control trees

Status: Sprint 13 implementation note. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Inventory

`gui.bif` holds 84 GUI resources (type 2047); `patch.erf` holds 7, of which
`mipc212x10` and `tooltip12x10` are patch-only, giving **86 unique screens**
and **91 resource occurrences**. The shipped set parses to **2,302 controls**
across 7 control types (0 control, 2 panel, 4 image, 5 label, 6 button,
7 checkbox, 8 slider, 9 scrollbar, 10 progress, 11 listbox); no unknown type
occurs. The largest screen has 121 controls.

## Format

A `.gui` file is GFF V3.2 with magic `GUI `:

- root `CONTROLS` list, `CONTROLTYPE`, `TAG`, `EXTENT`, `ALPHA`, `COLOR`;
- each control struct repeats `CONTROLTYPE`, `TAG`, `ID`, `EXTENT`;
- optional `CONTROLS` child list (leaf controls may omit it);
- `BORDER` / `HILIGHT` structs with `FILL`, `EDGE`, `CORNER` resrefs;
- `TEXT` struct with `TEXT` and `FONT`;
- control extents are in 640x480 GUI space.

`parseGui` recursively parses the control tree, resolves extents/colors/text/
border resrefs and counts controls. The runtime debug renderer draws every
control rectangle, tag/text and panel/button/list colors in GUI space; the
`mainmenu` gate screen loads 14 controls and renders at 960x540
(`docs/evidence/s13/gui-mainmenu.png`).

## Gate

`--gui-test` asserts:

- 86 unique screens (`screens=86`);
- 2,302 controls (`controls=2302`);
- all types are known (`control_types=1`);
- `mainmenu` is `TGuiPanel` with 14 controls (`mainmenu=1`);
- maximum control count 121.

## Evidence

- `docs/evidence/s13/gui.log`
- `docs/evidence/s13/gui-mainmenu.png`
- `engine/src/kotor/Gui.*`
- `engine/src/app/Application.cpp` (`--gui-screen` overlay)
