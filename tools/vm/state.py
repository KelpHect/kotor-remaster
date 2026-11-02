#!/usr/bin/env python3
"""Minimal deterministic object/world state for the independent NCS runtime.

This is deliberately a *headless* model: it implements the object identity,
tag/area, local/global variable, inventory/party, action-queue, and timer
surfaces that shipped scripts use, without calling the original engine.
Engine-side effects (combat, dialogue, rendering, audio) are mediated by the
action runtime and recorded, not executed.

No game data is read or written.
"""
from __future__ import annotations

import random
from collections import defaultdict, deque
from dataclasses import dataclass, field
from typing import Any

from vm.ncs_vm import OBJECT_INVALID, Value, default_cells


@dataclass
class GameObject:
    id: int
    tag: str
    area: int = 100
    locals: dict[str, Value] = field(default_factory=dict)
    inventory: list[int] = field(default_factory=list)
    base_item: int = 0
    valid: bool = True
    position: list[float] = field(default_factory=lambda: [0.0, 0.0, 0.0])
    hp: int = 10
    attrs: dict[str, Any] = field(default_factory=dict)


@dataclass
class Timer:
    due: float
    action_id: int
    args: list
    source: str = "DelayCommand"


@dataclass
class QueuedAction:
    action_id: int
    args: list
    at: float
    source: str = "Action"


class World:
    """Small deterministic world used by one script execution."""

    def __init__(self, seed: int = 0x4B4F544F52):
        self.next_id = 1
        self.objects: dict[int, GameObject] = {}
        self.tags: dict[str, list[int]] = defaultdict(list)
        self.globals: dict[str, Value] = {}
        self.party: list[int] = []
        self.action_queue: deque[QueuedAction] = deque()
        self.timers: list[Timer] = []
        self.journal: set[int] = set()
        self.log: list[tuple[float, str]] = []
        self.events: list[dict[str, Any]] = []
        self.attrs: dict[str, Any] = {
            "module": "end_m01aa",
            "difficulty": 1,
            "movie_playing": False,
            "planets": {"Taris": True, "Dantooine": False},
            "encounter_spawns_max": 10,
            "encounter_spawns_current": 0,
            "encounter_difficulty": 1,
            "stealth_xp": 0,
            "stealth_enabled": 1,
            "stealth_decrement": 5,
            "max_stealth_xp": 100,
            "journal_xp": {},
            "clicking_object": 0,
            "last_speaker": 0,
            "last_attacker": 0,
            "transition_target": 0,
            "starting_location": 0,
            "last_item_equipped": 0,
            "spell_cast_item": 0,
            "item_activated_target": 0,
            "item_activated_location": 0,
            "last_conversation": "",
            "matched_substring": "",
            "matched_count": 0,
            "last_player_dying": 0,
            "last_respawn_button_presser": 0,
            "last_trap_detected": 0,
            "module_item_acquired_from": 0,
            "module_item_lost": 0,
            "module_item_lost_by": 0,
            "last_perception_heard": 0,
            "last_perception_inaudible": 0,
            "last_closed_by": 0,
            "aoe_creator": 0,
            "last_associate_command": 0,
        }
        self.time = 0.0
        self.rng = random.Random(seed)
        self._cursors: dict[tuple, int] = defaultdict(int)
        self.pc = self.add_object("PC", area=100)
        self.party.append(self.pc.id)

    # ---- objects -------------------------------------------------------
    def add_object(self, tag: str, area: int = 100, base_item: int = 0,
                   valid: bool = True) -> GameObject:
        oid = self.next_id
        self.next_id += 1
        obj = GameObject(id=oid, tag=tag, area=area, base_item=base_item, valid=valid)
        self.objects[oid] = obj
        self.tags[tag].append(oid)
        return obj

    def get(self, oid: int) -> GameObject | None:
        return self.objects.get(int(oid))

    def valid(self, oid: int) -> bool:
        obj = self.get(oid)
        return bool(obj and obj.valid)

    def by_tag(self, tag: str, nth: int = 0) -> int:
        ids = self.tags.get(tag or "", [])
        try:
            nth = int(nth)
        except (TypeError, ValueError):
            nth = 0
        if 0 <= nth < len(ids):
            return ids[nth]
        return OBJECT_INVALID

    def by_area(self, area: int) -> list[int]:
        return [o.id for o in self.objects.values() if o.area == int(area) and o.valid]

    # ---- locals / globals ---------------------------------------------
    def get_local(self, oid: int, name: str, typ: str = "int") -> Value:
        obj = self.get(oid)
        if not obj:
            return default_cells(typ)[0]
        v = obj.locals.get(name or "")
        if v is None or v.typ != typ:
            return default_cells(typ)[0]
        return v

    def set_local(self, oid: int, name: str, value: Value) -> None:
        obj = self.get(oid)
        if obj:
            obj.locals[name or ""] = value

    def get_global(self, name: str, typ: str = "int") -> Value:
        v = self.globals.get(name or "")
        if v is None or v.typ != typ:
            return default_cells(typ)[0]
        return v

    def set_global(self, name: str, value: Value) -> None:
        self.globals[name or ""] = value

    # ---- cursors ------------------------------------------------------
    def cursor(self, key: tuple) -> int:
        return self._cursors[key]

    def set_cursor(self, key: tuple, value: int) -> None:
        self._cursors[key] = value

    # ---- queues / timers ----------------------------------------------
    def queue_action(self, action_id: int, args: list, source: str = "Action") -> None:
        self.action_queue.append(QueuedAction(action_id, args, self.time, source))

    def schedule(self, delay: float, action_id: int, args: list,
                 source: str = "DelayCommand") -> None:
        self.timers.append(Timer(self.time + float(delay or 0.0), action_id, args, source))

    def advance(self, seconds: float) -> list[Timer]:
        self.time += float(seconds or 0.0)
        due = [t for t in self.timers if t.due <= self.time]
        if due:
            self.timers = [t for t in self.timers if t.due > self.time]
        return due

    def summary(self) -> dict[str, Any]:
        return {
            "objects": len(self.objects),
            "valid_objects": sum(1 for o in self.objects.values() if o.valid),
            "globals": len(self.globals),
            "party": len(self.party),
            "queued_actions": len(self.action_queue),
            "timers": len(self.timers),
            "time": self.time,
        }
