"""Lane s1-03: map compiler + hard-fail validator over all 117 KotOR areas.

Read-only against the game install (KOTOR_GAME_DIR). Consumes original
module RIMs (ARE/GIT/IFO, PTH) and base BIFs (LYT/VIS/WOK/MDL/MDX) through
tools/resources/kotor_archive.py and tools/formats/gff.py; the reusable
binary/text parsers live in this package (tools/geometry is not modified).

Modules:
  constants   resource type ids, rule/error codes, frozen tolerances
  parsers     WOK(BWM), LYT, VIS, MDL/MDX node+mesh readers, GFF-bytes helper
  loader      GameStore + load_module -> ModuleData
  validator   hard-fail rules R1..R6 -> report dict
  compile     CLI: run over all modules, write per-area JSON + summary

Evidence base: build SHA-256 0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6
(docs/MASTER_PLAN.md §3); coordinate contract per docs/adr/0002 and
docs/recon/03, 04, 11, 12, 15 (see docs/recon/19_MAP_COMPILE.md).
"""

BUILD_SHA256 = "0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6"
PACKAGE_VERSION = "1.0.0"
