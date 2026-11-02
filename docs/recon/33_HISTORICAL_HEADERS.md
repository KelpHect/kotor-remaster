# 33 — Historical prototype headers for the default-argument residuals

Status: **closed** — 24/24 recompiled byte-identically from the unchanged clean-room source for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Gate

| check | result |
|---|---|
| Affected scripts found | **24/24** |
| Raw byte-identical recompiles | **24/24** |
| Failures | **0** |

## Method

The decompiled source call sites already have the historical arity. The shipped compiler omitted trailing defaults; `nwnnsscomp_ktool` materializes them from the prototype. For each script this tool builds a per-file `nwscript.nss` whose affected action prototype has exactly the historical parameter count, recompiles the unchanged NSS, and compares the bytes. No bytecode is rewritten.

- `k_pdan_28a_area__985285843c84e557` — DestroyObject argc=2 — **equal**
- `k_pebn_loc01__87612f468dfa59bd` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc02__fbd389031e0a490e` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc03__51243f0db9371e9c` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc04__f726aeb7d7ad5bba` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc05__cb2e57a8ee46ce30` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc06__4bd6404975f587b4` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc07__3d8db8c1259ea951` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc08__2d3d421b376b7adf` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_loc09__bcf36e47e7ebbd98` — ShowPartySelectionGUI argc=1 — **equal**
- `k_pebn_swoop__dd950256cb325014` — StartNewModule argc=4 — **equal**
- `k_pebo_death3__95c9c39449845acd` — EndGame argc=0 — **equal**
- `k_pebo_death__4132ffd6871a7533` — EndGame argc=0 — **equal**
- `k_pebo_trig2__18804f99c832e75a` — ActionStartConversation argc=5 — **equal**
- `k_ptar_compconv__a3802cae9787de81` — ActionStartConversation argc=5 — **equal**
- `k_ptar_labcs_en__8831f11e22b99ab2` — ActionStartConversation argc=5 — **equal**
- `k_ptar_pazvulk__170629d07badd63d` — PlayPazaak argc=4 — **equal**
- `k_ptar_pazvulken__b4479696b6acf1eb` — ActionStartConversation argc=5 — **equal**
- `k_ptar_vulkelev__28f489d5f6338745` — ActionStartConversation argc=5 — **equal**
- `k_ptat17aa_enter__bd2d42edc4453ce7` — ActionStartConversation argc=5 — **equal**
- `k_punk_41blkr_ud__cfd01ef1ad1ba586` — ActionStartConversation argc=5 — **equal**
- `k_trg_messengers__739e3d543c650ffc` — ActionStartConversation argc=5 — **equal**
- `k_trg_senni__2e19d3c4dda8074e` — ActionStartConversation argc=5 — **equal**
- `nw_g0_conversat__9c7c7ada35c6bb72` — BeginConversation argc=0 — **equal**

## Reproduce

```bash
python -m tools.scripts.historical_headers \
  --corpus artifacts/lanes/04-ncs/extracted/unique \
  --nss artifacts/lanes/ncs-decompiler/cont2/out/nss \
  --header artifacts/lanes/ncs-decompiler/cont2/runs/incwhile-v10/bin/nwscript.nss \
  --compiler artifacts/lanes/04-ncs/tools/DeNCS/tools/nwnnsscomp_ktool.exe \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --corpus-json artifacts/lanes/s1-04-scripts/corpus.json \
  --taxonomy artifacts/lanes/ncs-decompiler/cont2/out/residual-taxonomy.json \
  --out-json artifacts/lanes/s3-live/historical_headers.json \
  --out-doc docs/recon/33_HISTORICAL_HEADERS.md
```
