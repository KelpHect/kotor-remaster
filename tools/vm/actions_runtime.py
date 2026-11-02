#!/usr/bin/env python3
"""Stateful action runtime for the independent NCS VM.

The runtime gives every one of the 772 action ids a handler.  Data, object,
tag/area, inventory/party, local/global variable, string/vector, action-queue,
and timer actions have explicit behavior over ``tools/vm/state.py``.  Complex
engine effects (combat, dialogue, effects, audio, rendering) are mediated:
they resolve deterministically, update the headless queue/state when relevant,
and return a typed default.  No original engine code is called.

The VM passes ACTION arguments as ``list[list[Value]]`` (one cell list per
declared parameter; vector = 3 cells; functor = 0 cells).
"""
from __future__ import annotations

import math
import os
import re
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
sys.path.insert(0, _HERE)

from vm.ncs_vm import OBJECT_INVALID, Value, default_cells  # noqa: E402
from vm.state import World  # noqa: E402


class ActionRuntime:
    def __init__(self, specs: dict, world: World | None = None):
        self.specs = {a["id"]: a for a in specs["actions"]}
        self.name_by_id = {a["id"]: a["name"] for a in specs["actions"]}
        self.ret_by_id = {a["id"]: a["return"] for a in specs["actions"]}
        self.world = world or World()
        self.calls: Counter = Counter()
        self.specific: set[str] = set()
        self.generic: set[str] = set()
        self.default: set[str] = set()
        self.timers_scheduled = 0
        self.actions_queued = 0
        self.handlers = {}
        for a in specs["actions"]:
            fn = getattr(self, "h_" + a["name"], None)
            if fn is not None:
                self.handlers[a["name"]] = fn

    # ---- argument helpers ---------------------------------------------
    @staticmethod
    def _cell(args, i, typ="int") -> Value:
        if i < len(args) and args[i]:
            return args[i][0]
        return default_cells(typ)[0]

    def _i(self, args, i, typ="int") -> int:
        return self._cell(args, i, typ).as_int()

    def _f(self, args, i) -> float:
        return self._cell(args, i, "float").as_float()

    def _s(self, args, i) -> str:
        return self._cell(args, i, "string").as_str()

    def _o(self, args, i, default=OBJECT_INVALID) -> int:
        v = self._cell(args, i, "object")
        return v.as_int() if v.typ == "object" else default

    def _vec(self, args, i) -> list[Value]:
        if i < len(args) and len(args[i]) >= 3:
            return [Value("float", x.as_float()) for x in args[i][:3]]
        return [Value("float", 0.0) for _ in range(3)]

    def _ret(self, aid: int) -> list[Value]:
        typ = self.ret_by_id.get(aid, "void")
        return [] if typ == "void" else default_cells(typ)

    # ---- dispatch ------------------------------------------------------
    def call(self, aid: int, args: list, vm) -> list[Value]:
        name = self.name_by_id.get(aid)
        self.calls[aid] += 1
        if name is None:
            self.default.add(f"#{aid}")
            return self._ret(aid)
        fn = self.handlers.get(name)
        if fn is not None:
            self.specific.add(name)
            return fn(args, vm)
        cells = self._generic(name, args, vm, aid)
        return cells

    def _generic(self, name: str, args: list, vm, aid: int) -> list[Value]:
        if name.startswith("Action"):
            self.world.queue_action(aid, args, "Action")
            self.actions_queued += 1
            self.generic.add(name)
            return self._ret(aid)
        if name.startswith("d") and name[1:].isdigit():
            self.generic.add(name)
            return [Value("int", self.world.rng.randint(1, int(name[1:])))]
        if name.startswith(("Get", "Is", "Has", "Find")):
            self.generic.add(name)
            return self._ret(aid)
        if name.startswith(("Set", "Clear", "Stop", "Destroy", "Remove",
                            "Apply", "Change", "Face", "Jump", "Move",
                            "Delay", "Assign", "Play", "Sound", "Music",
                            "Speak", "Effect", "Print", "Send", "Show",
                            "Take", "Give", "SWMG_", "TUR_", "PAZ_",
                            "Add", "Adjust", "Ambient", "Aur", "Award",
                            "Bark", "Begin", "Cancel", "Create", "Disable",
                            "End", "Event", "Execute", "Location", "NoClicks",
                            "Object", "Queue", "Reveal", "Ship", "Signal",
                            "Spawn", "Start", "Surrender", "Switch", "Talent",
                            "Yavin")):
            self.generic.add(name)
            return self._ret(aid)
        self.default.add(name)
        return self._ret(aid)

    # ---- core object / tag / area --------------------------------------
    def h_GetTag(self, args, vm):
        obj = self.world.get(self._o(args, 0))
        return [Value("string", obj.tag if obj else "")]

    def h_GetObjectByTag(self, args, vm):
        oid = self.world.by_tag(self._s(args, 0), self._i(args, 1))
        return [Value("object", oid)]

    def h_GetWaypointByTag(self, args, vm):
        oid = self.world.by_tag(self._s(args, 0), 0)
        return [Value("object", oid)]

    def h_GetArea(self, args, vm):
        obj = self.world.get(self._o(args, 0))
        return [Value("object", obj.area if obj else OBJECT_INVALID)]

    def h_GetFirstPC(self, args, vm):
        return [Value("object", self.world.pc.id)]

    def h_GetPartyMemberByIndex(self, args, vm):
        idx = self._i(args, 0)
        if 0 <= idx < len(self.world.party):
            return [Value("object", self.world.party[idx])]
        return [Value("object", OBJECT_INVALID)]

    def h_GetIsObjectValid(self, args, vm):
        return [Value("int", 1 if self.world.valid(self._o(args, 0)) else 0)]

    def h_GetNextObjectInArea(self, args, vm):
        area = self._o(args, 0)
        if area == OBJECT_INVALID:
            area = self.world.pc.area
        key = ("area", area)
        ids = self.world.by_area(area)
        pos = self.world.cursor(key)
        if pos >= len(ids):
            return [Value("object", OBJECT_INVALID)]
        self.world.set_cursor(key, pos + 1)
        return [Value("object", ids[pos])]

    # ---- inventory / items ---------------------------------------------
    def h_GetNextItemInInventory(self, args, vm):
        oid = self._o(args, 0)
        if oid == OBJECT_INVALID:
            oid = self.world.pc.id
        obj = self.world.get(oid)
        key = ("inv", oid)
        inv = obj.inventory if obj else []
        pos = self.world.cursor(key)
        if pos >= len(inv):
            return [Value("object", OBJECT_INVALID)]
        self.world.set_cursor(key, pos + 1)
        return [Value("object", inv[pos])]

    def h_GetItemInSlot(self, args, vm):
        slot = self._i(args, 0)
        oid = self._o(args, 1)
        if oid == OBJECT_INVALID:
            oid = self.world.pc.id
        obj = self.world.get(oid)
        if obj and 0 <= slot < len(obj.inventory):
            return [Value("object", obj.inventory[slot])]
        return [Value("object", OBJECT_INVALID)]

    def h_GetBaseItemType(self, args, vm):
        obj = self.world.get(self._o(args, 0))
        return [Value("int", obj.base_item if obj else 0)]

    def h_GetGold(self, args, vm):
        return [Value("int", 0)]

    # ---- globals -------------------------------------------------------
    def h_GetGlobalNumber(self, args, vm):
        return [self.world.get_global(self._s(args, 0), "int")]

    def h_SetGlobalNumber(self, args, vm):
        self.world.set_global(self._s(args, 0), Value("int", self._i(args, 1)))
        return []

    def h_GetGlobalBoolean(self, args, vm):
        return [self.world.get_global(self._s(args, 0), "int")]

    def h_SetGlobalBoolean(self, args, vm):
        self.world.set_global(self._s(args, 0), Value("int", 1 if self._i(args, 1) else 0))
        return []

    def h_GetGlobalString(self, args, vm):
        return [self.world.get_global(self._s(args, 0), "string")]

    def h_SetGlobalString(self, args, vm):
        self.world.set_global(self._s(args, 0), Value("string", self._s(args, 1)))
        return []

    def h_GetGlobalLocation(self, args, vm):
        return [Value("engine", 0)]

    def h_SetGlobalLocation(self, args, vm):
        return []

    # ---- locals (K1 numeric indexes) -----------------------------------
    def _local_key(self, args, index):
        return str(self._i(args, index))

    def h_GetLocalNumber(self, args, vm):
        oid = self._o(args, 0)
        return [self.world.get_local(oid, self._local_key(args, 1), "int")]

    def h_SetLocalNumber(self, args, vm):
        oid = self._o(args, 0)
        self.world.set_local(oid, self._local_key(args, 1), Value("int", self._i(args, 2)))
        return []

    def h_GetLocalBoolean(self, args, vm):
        oid = self._o(args, 0)
        return [self.world.get_local(oid, self._local_key(args, 1), "int")]

    def h_SetLocalBoolean(self, args, vm):
        oid = self._o(args, 0)
        self.world.set_local(oid, self._local_key(args, 1),
                             Value("int", 1 if self._i(args, 2) else 0))
        return []

    # ---- math / strings / vectors --------------------------------------
    def h_Random(self, args, vm):
        n = self._i(args, 0)
        return [Value("int", self.world.rng.randrange(max(1, n)))]

    def h_abs(self, args, vm):
        return [Value("int", abs(self._i(args, 0)))]

    def h_VectorNormalize(self, args, vm):
        v = [x.as_float() for x in self._vec(args, 0)]
        mag = math.sqrt(sum(x * x for x in v))
        if mag == 0.0:
            return [Value("float", 0.0) for _ in range(3)]
        return [Value("float", x / mag) for x in v]

    # ---- math ----------------------------------------------------------
    def h_fabs(self, args, vm):
        return [Value("float", abs(self._f(args, 0)))]

    def h_cos(self, args, vm):
        return [Value("float", math.cos(self._f(args, 0)))]

    def h_sin(self, args, vm):
        return [Value("float", math.sin(self._f(args, 0)))]

    def h_tan(self, args, vm):
        return [Value("float", math.tan(self._f(args, 0)))]

    def h_acos(self, args, vm):
        try:
            return [Value("float", math.acos(self._f(args, 0)))]
        except ValueError:
            return [Value("float", 0.0)]

    def h_asin(self, args, vm):
        try:
            return [Value("float", math.asin(self._f(args, 0)))]
        except ValueError:
            return [Value("float", 0.0)]

    def h_atan(self, args, vm):
        return [Value("float", math.atan(self._f(args, 0)))]

    def h_log(self, args, vm):
        try:
            return [Value("float", math.log(self._f(args, 0)))]
        except ValueError:
            return [Value("float", 0.0)]

    def h_pow(self, args, vm):
        return [Value("float", math.pow(self._f(args, 0), self._f(args, 1)))]

    def h_sqrt(self, args, vm):
        v = self._f(args, 0)
        return [Value("float", math.sqrt(v) if v >= 0 else 0.0)]

    def h_VectorMagnitude(self, args, vm):
        v = [x.as_float() for x in self._vec(args, 0)]
        return [Value("float", math.sqrt(sum(x * x for x in v)))]

    def h_TurnsToSeconds(self, args, vm):
        return [Value("float", float(self._i(args, 0)) * 6.0)]

    def h_AngleToVector(self, args, vm):
        a = math.radians(self._f(args, 0))
        return [Value("float", math.cos(a)), Value("float", math.sin(a)),
                Value("float", 0.0)]

    def h_VectorToAngle(self, args, vm):
        v = self._vec(args, 0)
        x, y = v[0].as_float(), v[1].as_float()
        return [Value("float", math.degrees(math.atan2(y, x)))]

    def h_FeetToMeters(self, args, vm):
        return [Value("float", self._f(args, 0) * 0.3048)]

    def h_YardsToMeters(self, args, vm):
        return [Value("float", self._f(args, 0) * 0.9144)]

    # ---- strings -------------------------------------------------------
    def h_InsertString(self, args, vm):
        dest = self._s(args, 0)
        text = self._s(args, 1)
        pos = max(0, min(len(dest), self._i(args, 2)))
        return [Value("string", dest[:pos] + text + dest[pos:])]

    def h_StringToFloat(self, args, vm):
        try:
            return [Value("float", float(self._s(args, 0) or "0"))]
        except ValueError:
            return [Value("float", 0.0)]

    def h_IntToHexString(self, args, vm):
        return [Value("string", f"{self._i(args, 0) & 0xFFFFFFFF:x}")]

    def h_TestStringAgainstPattern(self, args, vm):
        pattern = self._s(args, 0)
        text = self._s(args, 1)
        rx = "^" + "".join(".*" if c == "*" else "." if c == "?" else
                           (re.escape(c)) for c in pattern) + "$"
        return [Value("int", 1 if re.match(rx, text) else 0)]

    def h_RandomName(self, args, vm):
        names = ("Carth", "Bastila", "Mission", "Zaalbar", "Jolee", "Juhani",
                 "Canderous", "T3-M4", "HK-47")
        return [Value("string", names[self.world.rng.randrange(len(names))])]

    def h_WriteTimestampedLogEntry(self, args, vm):
        self.world.log.append((self.world.time, self._s(args, 0)))
        return []

    # ---- effects / journal / UI ---------------------------------------
    def _effect(self, args, tag):
        return [Value("engine", tag)]

    def h_MagicalEffect(self, args, vm):
        return self._effect(args, 112)

    def h_SupernaturalEffect(self, args, vm):
        return self._effect(args, 113)

    def h_ExtraordinaryEffect(self, args, vm):
        return self._effect(args, 114)

    def h_VersusAlignmentEffect(self, args, vm):
        return self._effect(args, 355)

    def h_VersusRacialTypeEffect(self, args, vm):
        return self._effect(args, 356)

    def h_VersusTrapEffect(self, args, vm):
        return self._effect(args, 357)

    def h_DeleteJournalWorldAllEntries(self, args, vm):
        self.world.journal.clear()
        return []

    def h_DeleteJournalWorldEntry(self, args, vm):
        self.world.journal.discard(self._i(args, 0))
        return []

    def h_DeleteJournalWorldEntryStrref(self, args, vm):
        self.world.journal.discard(self._i(args, 0))
        return []

    def h_PopUpGUIPanel(self, args, vm):
        self.world.queue_action(388, args, "PopUpGUIPanel")
        self.actions_queued += 1
        return []

    def h_PopUpDeathGUIPanel(self, args, vm):
        self.world.queue_action(554, args, "PopUpDeathGUIPanel")
        self.actions_queued += 1
        return []

    def h_SuppressStatusSummaryEntry(self, args, vm):
        self.world.queue_action(763, args, "SuppressStatusSummaryEntry")
        self.actions_queued += 1
        return []

    def h_FloatingTextStrRefOnCreature(self, args, vm):
        self.world.queue_action(525, args, "FloatingTextStrRefOnCreature")
        self.actions_queued += 1
        return []

    def h_FloatingTextStringOnCreature(self, args, vm):
        self.world.queue_action(526, args, "FloatingTextStringOnCreature")
        self.actions_queued += 1
        return []

    def h_ExploreAreaForPlayer(self, args, vm):
        self.world.queue_action(403, args, "ExploreAreaForPlayer")
        self.actions_queued += 1
        return []

    def h_CutsceneMove(self, args, vm):
        obj = self.world.get(self._o(args, 0))
        if obj is not None:
            v = self._vec(args, 1)
            obj.position = [x.as_float() for x in v]
        self.world.queue_action(507, args, "CutsceneMove")
        self.actions_queued += 1
        return []

    def h_DoPlaceableObjectAction(self, args, vm):
        self.world.queue_action(547, args, "DoPlaceableObjectAction")
        self.actions_queued += 1
        return []

    def h_ExportAllCharacters(self, args, vm):
        self.world.queue_action(557, args, "ExportAllCharacters")
        self.actions_queued += 1
        return []

    def h_TouchAttackMelee(self, args, vm):
        target = self.world.get(self._o(args, 0))
        hit = 1 if target is not None and target.valid else 0
        return [Value("int", hit)]

    def h_TouchAttackRanged(self, args, vm):
        target = self.world.get(self._o(args, 0))
        hit = 1 if target is not None and target.valid else 0
        return [Value("int", hit)]

    def h_GetStringLength(self, args, vm):
        return [Value("int", len(self._s(args, 0)))]

    def h_GetStringLeft(self, args, vm):
        return [Value("string", self._s(args, 0)[:max(0, self._i(args, 1))])]

    def h_GetStringRight(self, args, vm):
        s = self._s(args, 0)
        n = max(0, self._i(args, 1))
        return [Value("string", s[-n:] if n else "")]

    def h_GetSubString(self, args, vm):
        s = self._s(args, 0)
        start = max(0, self._i(args, 1))
        count = max(0, self._i(args, 2))
        return [Value("string", s[start:start + count])]

    def h_FindSubString(self, args, vm):
        return [Value("int", self._s(args, 0).find(self._s(args, 1)))]

    def h_StringToInt(self, args, vm):
        try:
            return [Value("int", int(self._s(args, 0) or "0"))]
        except ValueError:
            return [Value("int", 0)]

    def h_FloatToInt(self, args, vm):
        return [Value("int", int(self._f(args, 0)))]

    def h_IntToFloat(self, args, vm):
        return [Value("float", float(self._i(args, 0)))]

    def h_IntToString(self, args, vm):
        return [Value("string", str(self._i(args, 0)))]

    def h_FloatToString(self, args, vm):
        decimals = self._i(args, 2)
        try:
            return [Value("string", f"{self._f(args, 0):.{max(0, decimals)}f}")]
        except (ValueError, TypeError):
            return [Value("string", "0")]

    def h_Vector(self, args, vm):
        return [Value("float", self._f(args, 0)),
                Value("float", self._f(args, 1)),
                Value("float", self._f(args, 2))]

    def h_GetPosition(self, args, vm):
        obj = self.world.get(self._o(args, 0))
        pos = obj.position if obj else [0.0, 0.0, 0.0]
        return [Value("float", pos[0]), Value("float", pos[1]), Value("float", pos[2])]

    def h_GetDistanceToObject(self, args, vm):
        a = self.world.get(self._o(args, 0))
        if not a:
            return [Value("float", 0.0)]
        dx = a.position[0] - self.world.pc.position[0]
        dy = a.position[1] - self.world.pc.position[1]
        dz = a.position[2] - self.world.pc.position[2]
        return [Value("float", math.sqrt(dx * dx + dy * dy + dz * dz))]

    def h_GetTime(self, args, vm):
        return [Value("float", self.world.time)]

    # ---- action queue / timers / mediation -----------------------------
    def h_ActionWait(self, args, vm):
        vm.status = "blocked"
        vm.blocked_action = 202
        return []

    def h_DelayCommand(self, args, vm):
        delay = self._f(args, 0)
        self.world.schedule(delay, -1, args, "DelayCommand")
        self.timers_scheduled += 1
        return []

    def h_AssignCommand(self, args, vm):
        self.world.queue_action(-1, args, "AssignCommand")
        self.actions_queued += 1
        return []

    def h_ActionPauseConversation(self, args, vm):
        self.world.queue_action(205, args, "ActionPauseConversation")
        self.actions_queued += 1
        return []

    def h_SetListenPattern(self, args, vm):
        self.world.queue_action(176, args, "SetListenPattern")
        self.actions_queued += 1
        return []

    def h_GetLocalString(self, args, vm):
        oid = self._o(args, 0)
        return [self.world.get_local(oid, self._local_key(args, 1), "string")]

    def h_SetLocalString(self, args, vm):
        oid = self._o(args, 0)
        self.world.set_local(oid, self._local_key(args, 1), Value("string", self._s(args, 2)))
        return []

    def summary(self) -> dict:
        return {
            "calls": sum(self.calls.values()),
            "unique_actions": len(self.calls),
            "specific": sorted(self.specific),
            "generic": sorted(self.generic),
            "default": sorted(self.default),
            "timers": len(self.world.timers),
            "queued_actions": len(self.world.action_queue),
            "world": self.world.summary(),
        }
