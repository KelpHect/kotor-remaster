# 30 — Unused-action conformance (the 306 ids outside the shipped corpus)

Status: **closed** — 306/306 unused action ids invoked through the functional runtime for build SHA-256 `0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Gate

| check | result |
|---|---|
| Unused ids checked | **306/306** |
| Specific handlers | **42** |
| Generic semantic handlers | **264** |
| Unclassified/default | **0** |
| Errors | **0** |
| Gate | **closed** |

## Categories

| category | actions |
|---|---:|
| UI/floating text | 5 |
| action queue | 10 |
| effect wrapper | 6 |
| engine-mediated | 8 |
| journal | 3 |
| math | 11 |
| mediated engine effect | 144 |
| query/read | 110 |
| string | 3 |
| vector/math | 6 |

## Method

- The corpus usage ledger (`artifacts/lanes/s1-04-scripts/action_usage_report.json`)
  lists 466 actions used by the 8,666 shipped scripts; the other 306 are listed here.
- For each unused id, a synthetic ACTION is built with the prototype's declared
  parameter count and a typed default for each parameter (vector 3 cells, functor 0).
- The action is dispatched through `tools/vm/actions_runtime.py`; the return cell count
  is checked against the prototype (void 0, vector 3, everything else 1).
- `specific` handlers implement the action directly; `generic` handlers apply a
  documented semantic group (query, state/effect, action queue, UI, journal, audio,
  minigame, math/string). `default` is the unclassified fallback and must be zero.
- Behavioral conformance (prepared world, state-derived values, queue/mediation
  records) is in `docs/recon/34_ACTION_BEHAVIOR.md`.

## Full list

| id | action | return | handler | category |
|---:|---|---|---|---|
| 2 | PrintFloat | void | generic | mediated engine effect |
| 4 | PrintInteger | void | generic | mediated engine effect |
| 5 | PrintObject | void | generic | mediated engine effect |
| 12 | SetTime | void | generic | mediated engine effect |
| 34 | ActionPickUpItem | void | generic | action queue |
| 35 | ActionPutDownItem | void | generic | action queue |
| 39 | ActionSpeakString | void | generic | action queue |
| 45 | SetCameraFacing | void | generic | mediated engine effect |
| 52 | GetLastItemEquipped | object | generic | query/read |
| 60 | GetStringUpperCase | string | generic | query/read |
| 61 | GetStringLowerCase | string | generic | query/read |
| 64 | InsertString | string | specific | string |
| 67 | fabs | float | specific | math |
| 68 | cos | float | specific | math |
| 69 | sin | float | specific | math |
| 70 | tan | float | specific | math |
| 71 | acos | float | specific | math |
| 72 | asin | float | specific | math |
| 73 | atan | float | specific | math |
| 74 | log | float | specific | math |
| 75 | pow | float | specific | math |
| 76 | sqrt | float | specific | math |
| 89 | GetEffectDurationType | int | generic | query/read |
| 90 | GetEffectSubType | int | generic | query/read |
| 104 | VectorMagnitude | float | specific | vector/math |
| 112 | MagicalEffect | effect | specific | effect wrapper |
| 113 | SupernaturalEffect | effect | specific | effect wrapper |
| 114 | ExtraordinaryEffect | effect | specific | effect wrapper |
| 116 | GetAC | int | generic | query/read |
| 119 | EffectDamageReduction | effect | generic | mediated engine effect |
| 123 | TurnsToSeconds | float | specific | vector/math |
| 127 | GetAlignmentGoodEvil | int | generic | query/read |
| 134 | EffectKnockdown | effect | generic | mediated engine effect |
| 141 | PrintVector | void | generic | mediated engine effect |
| 144 | AngleToVector | vector | specific | vector/math |
| 145 | VectorToAngle | float | specific | vector/math |
| 146 | TouchAttackMelee | int | specific | engine-mediated |
| 147 | TouchAttackRanged | int | specific | engine-mediated |
| 149 | EffectSpellImmunity | effect | generic | mediated engine effect |
| 153 | EffectForceJump | effect | generic | mediated engine effect |
| 157 | EffectConfused | effect | generic | mediated engine effect |
| 158 | EffectFrightened | effect | generic | mediated engine effect |
| 167 | ActionForceFollowObject | void | generic | action queue |
| 171 | EffectAreaOfEffect | effect | generic | mediated engine effect |
| 173 | ChangeFaction | void | generic | mediated engine effect |
| 174 | GetIsListening | int | generic | query/read |
| 177 | TestStringAgainstPattern | int | specific | engine-mediated |
| 178 | GetMatchedSubstring | string | generic | query/read |
| 179 | GetMatchedSubstringsCount | int | generic | query/read |
| 181 | GetFactionWeakestMember | object | generic | query/read |
| 182 | GetFactionStrongestMember | object | generic | query/read |
| 183 | GetFactionMostDamagedMember | object | generic | query/read |
| 184 | GetFactionLeastDamagedMember | object | generic | query/read |
| 185 | GetFactionGold | int | generic | query/read |
| 187 | GetFactionAverageGoodEvilAlignment | int | generic | query/read |
| 188 | SoundObjectGetFixedVariance | float | generic | mediated engine effect |
| 189 | GetFactionAverageLevel | int | generic | query/read |
| 190 | GetFactionAverageXP | int | generic | query/read |
| 191 | GetFactionMostFrequentClass | int | generic | query/read |
| 192 | GetFactionWorstAC | object | generic | query/read |
| 193 | GetFactionBestAC | object | generic | query/read |
| 198 | GetTransitionTarget | object | generic | query/read |
| 203 | SetAreaTransitionBMP | void | generic | mediated engine effect |
| 208 | GetReputation | int | generic | query/read |
| 211 | GetGoingToBeAttackedBy | object | generic | query/read |
| 218 | FeetToMeters | float | specific | vector/math |
| 219 | YardsToMeters | float | specific | vector/math |
| 225 | GetFacingFromLocation | float | generic | query/read |
| 226 | GetNearestCreatureToLocation | object | generic | query/read |
| 228 | GetNearestObjectToLocation | object | generic | query/read |
| 233 | StringToFloat | float | specific | string |
| 234 | ActionCastSpellAtLocation | void | generic | action queue |
| 240 | ActionSpeakStringByStrRef | void | generic | action queue |
| 249 | RandomName | string | specific | math |
| 252 | EffectAssuredDeflection | effect | generic | mediated engine effect |
| 257 | GetLastPerceptionHeard | int | generic | query/read |
| 258 | GetLastPerceptionInaudible | int | generic | query/read |
| 260 | GetLastClosedBy | object | generic | query/read |
| 264 | GetAreaOfEffectCreator | object | generic | query/read |
| 267 | GetButtonMashCheck | int | generic | query/read |
| 270 | EffectHaste | effect | generic | mediated engine effect |
| 278 | GetEncounterSpawnsMax | int | generic | query/read |
| 279 | SetEncounterSpawnsMax | void | generic | mediated engine effect |
| 280 | GetEncounterSpawnsCurrent | int | generic | query/read |
| 281 | SetEncounterSpawnsCurrent | void | generic | mediated engine effect |
| 283 | GetModuleItemAcquiredFrom | object | generic | query/read |
| 287 | ActionUseFeat | void | generic | action queue |
| 290 | GetObjectHeard | int | generic | query/read |
| 292 | GetModuleItemLost | object | generic | query/read |
| 293 | GetModuleItemLostBy | object | generic | query/read |
| 295 | EventConversation | event | generic | mediated engine effect |
| 296 | SetEncounterDifficulty | void | generic | mediated engine effect |
| 297 | GetEncounterDifficulty | int | generic | query/read |
| 298 | GetDistanceBetweenLocations | float | generic | query/read |
| 303 | TalentSkill | talent | generic | mediated engine effect |
| 310 | ActionUseTalentAtLocation | void | generic | action queue |
| 311 | GetGoldPieceValue | int | generic | query/read |
| 312 | GetIsPlayableRacialType | int | generic | query/read |
| 317 | GetLastAttackType | int | generic | query/read |
| 318 | GetLastAttackMode | int | generic | query/read |
| 321 | GetLastAssociateCommand | int | generic | query/read |
| 326 | GetClickingObject | object | generic | query/read |
| 327 | SetAssociateListenPatterns | void | generic | mediated engine effect |
| 328 | GetLastWeaponUsed | object | generic | query/read |
| 332 | GetIdentified | int | generic | query/read |
| 333 | SetIdentified | void | generic | mediated engine effect |
| 334 | GetDistanceBetweenLocations2D | float | generic | query/read |
| 347 | GetLastDisarmed | object | generic | query/read |
| 348 | GetLastDisturbed | object | generic | query/read |
| 349 | GetLastLocked | object | generic | query/read |
| 350 | GetLastUnlocked | object | generic | query/read |
| 355 | VersusAlignmentEffect | effect | specific | effect wrapper |
| 356 | VersusRacialTypeEffect | effect | specific | effect wrapper |
| 357 | VersusTrapEffect | effect | specific | effect wrapper |
| 360 | ActionMoveAwayFromLocation | void | generic | action queue |
| 371 | StopRumblePattern | int | generic | mediated engine effect |
| 374 | SendMessageToPC | void | generic | mediated engine effect |
| 380 | GetFirstFactionMember | object | generic | query/read |
| 381 | GetNextFactionMember | object | generic | query/read |
| 384 | GetJournalQuestExperience | int | generic | query/read |
| 387 | EffectHitPointChangeWhenDying | effect | generic | mediated engine effect |
| 388 | PopUpGUIPanel | void | specific | UI/floating text |
| 396 | IntToHexString | string | specific | string |
| 398 | GetItemHasItemProperty | int | generic | query/read |
| 401 | GetItemACValue | int | generic | query/read |
| 403 | ExploreAreaForPlayer | void | specific | engine-mediated |
| 404 | ActionEquipMostEffectiveArmor | void | generic | action queue |
| 405 | GetIsDay | int | generic | query/read |
| 406 | GetIsNight | int | generic | query/read |
| 407 | GetIsDawn | int | generic | query/read |
| 408 | GetIsDusk | int | generic | query/read |
| 410 | GetLastPlayerDying | object | generic | query/read |
| 411 | GetStartingLocation | location | generic | query/read |
| 416 | SoundObjectSetPosition | void | generic | mediated engine effect |
| 417 | SpeakOneLinerConversation | void | generic | mediated engine effect |
| 419 | GetLastRespawnButtonPresser | object | generic | query/read |
| 422 | GetIsWeaponEffective | int | generic | query/read |
| 424 | EventActivateItem | event | generic | mediated engine effect |
| 427 | MusicBackgroundSetDelay | void | generic | mediated engine effect |
| 432 | MusicBattleChange | void | generic | mediated engine effect |
| 435 | AmbientSoundChangeDay | void | generic | mediated engine effect |
| 436 | AmbientSoundChangeNight | void | generic | mediated engine effect |
| 438 | GetSpellCastItem | object | generic | query/read |
| 441 | GetItemActivatedTargetLocation | location | generic | query/read |
| 442 | GetItemActivatedTarget | object | generic | query/read |
| 448 | EffectDamageDecrease | effect | generic | mediated engine effect |
| 449 | EffectDamageImmunityDecrease | effect | generic | mediated engine effect |
| 453 | EffectSkillDecrease | effect | generic | mediated engine effect |
| 454 | EffectForceResistanceDecrease | effect | generic | mediated engine effect |
| 458 | EffectConcealment | effect | generic | mediated engine effect |
| 460 | EffectDispelMagicAll | effect | generic | mediated engine effect |
| 464 | GetMaxStealthXP | int | generic | query/read |
| 466 | EffectSeeInvisible | effect | generic | mediated engine effect |
| 467 | EffectTimeStop | effect | generic | mediated engine effect |
| 470 | EffectBlasterDeflectionDecrease | effect | generic | mediated engine effect |
| 472 | EffectSpellLevelAbsorption | effect | generic | mediated engine effect |
| 473 | EffectDispelMagicBest | effect | generic | mediated engine effect |
| 477 | EffectMissChance | effect | generic | mediated engine effect |
| 481 | GetStealthXPEnabled | int | generic | query/read |
| 486 | GetLastTrapDetected | object | generic | query/read |
| 487 | EffectDamageShield | effect | generic | mediated engine effect |
| 488 | GetNearestTrapToObject | object | generic | query/read |
| 491 | GetFortitudeSavingThrow | int | generic | query/read |
| 492 | GetWillSavingThrow | int | generic | query/read |
| 493 | GetReflexSavingThrow | int | generic | query/read |
| 494 | GetChallengeRating | float | generic | query/read |
| 496 | GetMovementRate | int | generic | query/read |
| 498 | GetStealthXPDecrement | int | generic | query/read |
| 499 | SetStealthXPDecrement | void | generic | mediated engine effect |
| 504 | SetCameraMode | void | generic | mediated engine effect |
| 507 | CutsceneMove | void | specific | engine-mediated |
| 511 | GetWeaponRanged | int | generic | query/read |
| 513 | GetGameDifficulty | int | generic | query/read |
| 516 | SetTutorialWindowsEnabled | void | generic | mediated engine effect |
| 521 | SWMG_GetLateralAccelerationPerSecond | float | generic | mediated engine effect |
| 523 | GetDifficultyModifier | float | generic | query/read |
| 525 | FloatingTextStrRefOnCreature | void | specific | UI/floating text |
| 526 | FloatingTextStringOnCreature | void | specific | UI/floating text |
| 527 | GetTrapDisarmable | int | generic | query/read |
| 528 | GetTrapDetectable | int | generic | query/read |
| 529 | GetTrapDetectedBy | int | generic | query/read |
| 530 | GetTrapFlagged | int | generic | query/read |
| 532 | GetTrapOneShot | int | generic | query/read |
| 533 | GetTrapCreator | object | generic | query/read |
| 534 | GetTrapKeyTag | string | generic | query/read |
| 535 | GetTrapDisarmDC | int | generic | query/read |
| 536 | GetTrapDetectDC | int | generic | query/read |
| 537 | GetLockKeyRequired | int | generic | query/read |
| 538 | GetLockKeyTag | int | generic | query/read |
| 539 | GetLockLockable | int | generic | query/read |
| 540 | GetLockUnlockDC | int | generic | query/read |
| 541 | GetLockLockDC | int | generic | query/read |
| 542 | GetPCLevellingUp | object | generic | query/read |
| 543 | GetHasFeatEffect | int | generic | query/read |
| 544 | SetPlaceableIllumination | void | generic | mediated engine effect |
| 545 | GetPlaceableIllumination | int | generic | query/read |
| 546 | GetIsPlaceableObjectActionPossible | int | generic | query/read |
| 547 | DoPlaceableObjectAction | void | specific | engine-mediated |
| 550 | SetTrapDetectedBy | int | generic | mediated engine effect |
| 551 | GetIsTrapped | int | generic | query/read |
| 553 | FaceObjectAwayFromObject | void | generic | mediated engine effect |
| 554 | PopUpDeathGUIPanel | void | specific | UI/floating text |
| 555 | SetTrapDisabled | void | generic | mediated engine effect |
| 557 | ExportAllCharacters | void | specific | engine-mediated |
| 558 | MusicBackgroundGetDayTrack | int | generic | mediated engine effect |
| 559 | MusicBackgroundGetNightTrack | int | generic | mediated engine effect |
| 560 | WriteTimestampedLogEntry | void | specific | engine-mediated |
| 561 | GetModuleName | string | generic | query/read |
| 562 | GetFactionLeader | object | generic | query/read |
| 567 | AmbientSoundSetDayVolume | void | generic | mediated engine effect |
| 568 | AmbientSoundSetNightVolume | void | generic | mediated engine effect |
| 571 | GetStrRefSoundDuration | float | generic | query/read |
| 583 | SWMG_GetLastEvent | string | generic | mediated engine effect |
| 584 | SWMG_GetLastEventModelName | string | generic | mediated engine effect |
| 585 | SWMG_GetObjectByName | object | generic | mediated engine effect |
| 587 | SWMG_GetLastBulletHitDamage | int | generic | mediated engine effect |
| 588 | SWMG_GetLastBulletHitTarget | int | generic | mediated engine effect |
| 589 | SWMG_GetLastBulletHitShooter | object | generic | mediated engine effect |
| 591 | SWMG_OnBulletHit | void | generic | mediated engine effect |
| 592 | SWMG_OnObstacleHit | void | generic | mediated engine effect |
| 594 | SWMG_GetLastObstacleHit | object | generic | mediated engine effect |
| 595 | SWMG_GetLastBulletFiredDamage | int | generic | mediated engine effect |
| 596 | SWMG_GetLastBulletFiredTarget | int | generic | mediated engine effect |
| 597 | SWMG_GetObjectName | string | generic | mediated engine effect |
| 599 | SWMG_IsFollower | int | generic | mediated engine effect |
| 600 | SWMG_IsPlayer | int | generic | mediated engine effect |
| 601 | SWMG_IsEnemy | int | generic | mediated engine effect |
| 602 | SWMG_IsTrigger | int | generic | mediated engine effect |
| 603 | SWMG_IsObstacle | int | generic | mediated engine effect |
| 606 | SWMG_GetLastHPChange | int | generic | mediated engine effect |
| 608 | SWMG_GetCameraNearClip | float | generic | mediated engine effect |
| 609 | SWMG_GetCameraFarClip | float | generic | mediated engine effect |
| 610 | SWMG_SetCameraClip | void | generic | mediated engine effect |
| 614 | SWMG_GetObstacleCount | int | generic | mediated engine effect |
| 615 | SWMG_GetObstacle | object | generic | mediated engine effect |
| 617 | SWMG_GetMaxHitPoints | int | generic | mediated engine effect |
| 618 | SWMG_SetMaxHitPoints | void | generic | mediated engine effect |
| 619 | SWMG_GetSphereRadius | float | generic | mediated engine effect |
| 620 | SWMG_SetSphereRadius | void | generic | mediated engine effect |
| 621 | SWMG_GetNumLoops | int | generic | mediated engine effect |
| 622 | SWMG_SetNumLoops | void | generic | mediated engine effect |
| 624 | SWMG_GetGunBankCount | int | generic | mediated engine effect |
| 625 | SWMG_GetGunBankBulletModel | string | generic | mediated engine effect |
| 626 | SWMG_GetGunBankGunModel | string | generic | mediated engine effect |
| 627 | SWMG_GetGunBankDamage | int | generic | mediated engine effect |
| 628 | SWMG_GetGunBankTimeBetweenShots | float | generic | mediated engine effect |
| 629 | SWMG_GetGunBankLifespan | float | generic | mediated engine effect |
| 630 | SWMG_GetGunBankSpeed | float | generic | mediated engine effect |
| 631 | SWMG_GetGunBankTarget | int | generic | mediated engine effect |
| 632 | SWMG_SetGunBankBulletModel | void | generic | mediated engine effect |
| 633 | SWMG_SetGunBankGunModel | void | generic | mediated engine effect |
| 634 | SWMG_SetGunBankDamage | void | generic | mediated engine effect |
| 635 | SWMG_SetGunBankTimeBetweenShots | void | generic | mediated engine effect |
| 636 | SWMG_SetGunBankLifespan | void | generic | mediated engine effect |
| 637 | SWMG_SetGunBankSpeed | void | generic | mediated engine effect |
| 638 | SWMG_SetGunBankTarget | void | generic | mediated engine effect |
| 639 | SWMG_GetLastBulletHitPart | string | generic | mediated engine effect |
| 640 | SWMG_IsGunBankTargetting | int | generic | mediated engine effect |
| 642 | SWMG_GetPlayerInvincibility | float | generic | mediated engine effect |
| 644 | SWMG_GetPlayerMinSpeed | float | generic | mediated engine effect |
| 646 | SWMG_GetPlayerTunnelPos | vector | generic | mediated engine effect |
| 647 | SWMG_SetPlayerOffset | void | generic | mediated engine effect |
| 648 | SWMG_SetPlayerInvincibility | void | generic | mediated engine effect |
| 653 | SWMG_GetPlayerTunnelNeg | vector | generic | mediated engine effect |
| 655 | SWMG_GetPlayerOrigin | vector | generic | mediated engine effect |
| 656 | SWMG_SetPlayerOrigin | void | generic | mediated engine effect |
| 657 | SWMG_GetGunBankHorizontalSpread | float | generic | mediated engine effect |
| 658 | SWMG_GetGunBankVerticalSpread | float | generic | mediated engine effect |
| 659 | SWMG_GetGunBankSensingRadius | float | generic | mediated engine effect |
| 660 | SWMG_GetGunBankInaccuracy | float | generic | mediated engine effect |
| 661 | SWMG_SetGunBankHorizontalSpread | void | generic | mediated engine effect |
| 662 | SWMG_SetGunBankVerticalSpread | void | generic | mediated engine effect |
| 663 | SWMG_SetGunBankSensingRadius | void | generic | mediated engine effect |
| 664 | SWMG_SetGunBankInaccuracy | void | generic | mediated engine effect |
| 667 | SWMG_GetPlayerMaxSpeed | float | generic | mediated engine effect |
| 669 | AddJournalWorldEntry | void | generic | mediated engine effect |
| 670 | AddJournalWorldEntryStrref | void | generic | mediated engine effect |
| 672 | DeleteJournalWorldAllEntries | void | specific | journal |
| 673 | DeleteJournalWorldEntry | void | specific | journal |
| 674 | DeleteJournalWorldEntryStrref | void | specific | journal |
| 675 | EffectForceDrain | effect | generic | mediated engine effect |
| 678 | SetJournalQuestEntryPicture | void | generic | mediated engine effect |
| 683 | SWMG_GetSoundFrequency | int | generic | mediated engine effect |
| 684 | SWMG_SetSoundFrequency | void | generic | mediated engine effect |
| 685 | SWMG_GetSoundFrequencyIsRandom | int | generic | mediated engine effect |
| 686 | SWMG_SetSoundFrequencyIsRandom | void | generic | mediated engine effect |
| 687 | SWMG_GetSoundVolume | int | generic | mediated engine effect |
| 688 | SWMG_SetSoundVolume | void | generic | mediated engine effect |
| 690 | SoundObjectSetPitchVariance | void | generic | mediated engine effect |
| 691 | SoundObjectGetVolume | int | generic | mediated engine effect |
| 706 | SetPartyAIStyle | void | generic | mediated engine effect |
| 711 | GetLastConversation | string | generic | query/read |
| 717 | SWMG_GetPlayerTunnelInfinite | vector | generic | mediated engine effect |
| 718 | SWMG_SetPlayerTunnelInfinite | void | generic | mediated engine effect |
| 727 | GetFirstAttacker | object | generic | query/read |
| 728 | GetNextAttacker | object | generic | query/read |
| 729 | SetFormation | void | generic | mediated engine effect |
| 731 | SetForcePowerUnsuccessful | void | generic | mediated engine effect |
| 741 | GetPlanetSelectable | int | generic | query/read |
| 743 | GetPlanetAvailable | int | generic | query/read |
| 746 | SetAreaFogColor | void | generic | mediated engine effect |
| 750 | SetGoodEvilValue | void | generic | mediated engine effect |
| 754 | EffectCutSceneHorrified | effect | generic | mediated engine effect |
| 756 | EffectCutSceneStunned | effect | generic | mediated engine effect |
| 763 | SuppressStatusSummaryEntry | void | specific | UI/floating text |
| 768 | IsMoviePlaying | int | generic | query/read |

## Reproduce

```bash
python tools/vm/unused_actions.py \
  --specs artifacts/lanes/s2-01-engine-map/action_specs.json \
  --usage artifacts/lanes/s1-04-scripts/action_usage_report.json \
  --out-json artifacts/lanes/s3-02-actions/unused_actions.json \
  --out-doc docs/recon/30_UNUSED_ACTIONS.md
```

