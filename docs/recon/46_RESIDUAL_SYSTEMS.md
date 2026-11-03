# Recon 46 - residual systems: loot, companion combat, merchant depth

Status: Sprint 8.5 evidence note. Target install: `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## 1. Container loot

UTP (`2044`) placeables carry an `ItemList`; each struct has `InventoryRes` (plus optional grid
position fields). `loadEntityAssets` collects the resrefs into `Entity::lootItems`. A placeable
with items is usable even if its `Useable` flag is 0.

`GameSession::openLoot` resolves each resref through the cached UTI loader into an `InventoryItem`,
opens `UiPanel::Loot`, and keeps `lootEntity_`. `takeLootItem` adds the item to the character
inventory, removes the first matching entry from both the loot list and the entity's `lootItems`,
and closes the panel when empty. `takeAllLoot` repeats it.

Test: `danm13` `FootLker` with one item; use, take all, inventory +1, loot empty.

## 2. Companion combat AI

`updateParty` now checks `combatTarget_` first. For each non-leader companion:

- if the target is farther than 2.2 m, close at 3.2 m/s using `wallBlocked`/`floorHeight`;
- otherwise, every 2 seconds, resolve a d20 attack from the companion's own `CharacterSheet`
  against the target's defense, roll 1d8 + STR damage, log `"<tag> hits <target>"`, and mark a
  defeated target dead.

Formation remains the fallback when no target is active. Leader switching and the 80% XP share are
unchanged.

## 3. Merchant gold and identification

`MerchantState::gold` is initialized from stock value (`max(1000, sum(cost*stack)/2)`), since the
shipped UTM/GIT store records do not carry a gold or faction field. Buy adds the price to store gold
and subtracts it from the player; sell requires `store.gold >= price` and moves the price back.
This is an explicit engine rule, not a claim of original 1:1 merchant gold behavior.

`InventoryItem::identified` comes from UTI `Identified`. `identifyInventoryItem` charges half the
item cost (minimum 1), sets the flag, and is exposed as an Identify button on unidentified merchant
inventory rows. Item lines in save schema 3 carry the flag; merchant gold has its own
`merchant_gold=<store>|<gold>` line.

## 4. Persistence and regression

The save now covers store gold, finite stock, item identification, companions, leader, feats,
powers, character, journal, inventory, globals and locals. VM corpus parity, 117-module sweep and
GL/Vulkan parity are unchanged.

## 5. Not closed here

- Delayed functor execution (`DelayCommand`/`AssignCommand`) needs a VM functor contract that does
  not destabilize the 8,666-script bytecode parity; dedicated S9 work.
- Merchant reputation haggling needs an explicit rule design because shipped store records have no
  faction field.
- Original saves, animation/skinning/weapons, cutscene cameras, audio/EAX, Bink/lip sync, full
  `.gui`, minigames, localization, mods/Override precedence and the P6 content checklist remain
  S9+.
