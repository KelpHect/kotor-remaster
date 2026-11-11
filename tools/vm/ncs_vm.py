#!/usr/bin/env python3
"""Independent NCS V1.0 virtual machine (KotOR 1.0.3.0).

Executes the documented NWScript stack machine:

* 4-byte typed stack cells, stack offsets ``len(stack) + off/4``;
* a separate JSR/RETN return stack (limit 128);
* ``SAVEBP``/``RESTOREBP`` globals and ``CPTOPBP``/``CPDOWNBP``;
* all ``0x01-0x2D`` opcodes, including ``DESTRUCT`` and ``STORESTATE``;
* ACTION mediation: every action id resolves to a typed default, so no engine
  code is entered and no game data is written.

The VM is intentionally permissive about missing optional action arguments and
extra arguments, matching the shipped compiler's observable convention.  It is
strict about unknown opcodes, unknown action ids, and stack bounds.

CLI:
    python tools/vm/ncs_vm.py --one <file.ncs> --specs <action_specs.json>
    python tools/vm/ncs_vm.py --corpus <dir> --specs <action_specs.json>
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, ".."))
from scripts import ncs  # noqa: E402

TYPE_BY_QUAL = {
    0x03: "int", 0x04: "float", 0x05: "string", 0x06: "object",
    0x10: "effect", 0x11: "event", 0x12: "location", 0x13: "talent",
}
QUAL_TYPE = {0x20: "int", 0x21: "float", 0x22: "object", 0x23: "string"}
STRUCT_SLOTS = {"vector": 3}
OBJECT_INVALID = 0x7F000000
# Retail fails a run once its instruction counter would reach 0x20000 (005d2bd0).
RETAIL_BUDGET = 0x1FFFF


class VMError(RuntimeError):
    pass


@dataclass
class Value:
    typ: str
    value: object

    def as_int(self) -> int:
        if self.typ == "float":
            return int(self.value)
        if self.typ == "string":
            return 0
        return int(self.value or 0)

    def as_float(self) -> float:
        return float(self.value or 0)

    def as_str(self) -> str:
        return self.value if self.typ == "string" else ""

    def truthy(self) -> bool:
        if self.typ == "string":
            return bool(self.value)
        if self.typ == "float":
            return self.value != 0.0
        if self.typ == "object":
            return self.value not in (0, OBJECT_INVALID)
        return int(self.value or 0) != 0


def default_cells(typ: str) -> list[Value]:
    if typ == "float":
        return [Value("float", 0.0)]
    if typ == "string":
        return [Value("string", "")]
    if typ == "object":
        return [Value("object", OBJECT_INVALID)]
    if typ in ("effect", "event", "location", "talent"):
        return [Value("engine", 0)]
    if typ == "vector":
        return [Value("float", 0.0) for _ in range(3)]
    return [Value("int", 0)]


def _i32(value: int) -> int:
    """The retail cell is 32 bits: integers wrap (005d2bd0)."""
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def _f32(value: float) -> float:
    """The retail cell is single precision (005d2bd0)."""
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return float("inf") if value > 0 else float("-inf")


def _bits(cell: "Value") -> int:
    """What EQUAL compares for int, float and object cells (005d2bd0)."""
    if cell.typ == "float":
        return struct.unpack("<I", struct.pack("<f", cell.as_float()))[0]
    if cell.typ == "string":
        return -1
    return int(cell.value or 0) & 0xFFFFFFFF


def _cell_offset(offset: int) -> int:
    """`SP - ((-offset) >> 2)` in retail."""
    return -((-offset) >> 2)


def _cell_count(size: int) -> int:
    """A signed 16-bit byte count, shifted (005d2bd0)."""
    if size >= 0x8000:
        size -= 0x10000
    return size >> 2


class VM:
    def __init__(self, instructions: list[ncs.Instruction], specs: dict,
                 budget: int = RETAIL_BUDGET, blocking_actions=(202,), runtime=None):
        self.ins = instructions
        self.specs = {a["id"]: a for a in specs["actions"]}
        self.budget = budget
        self.blocking_actions = set(blocking_actions)
        self.runtime = runtime
        self.stack: list[Value] = []
        self.bp = 0
        self.call_stack: list[int] = []
        self.ip = 0
        self.steps = 0
        self.max_sp = 0
        self.pending_functor = False
        self.action_counts: Counter = Counter()
        self.notes: Counter = Counter()
        self.status = None
        self.error = None
        self.error_ip = None
        self.blocked_action = None
        self.current_ins = None
        self.by_off = {i.offset: k for k, i in enumerate(instructions)}

    # ---- stack helpers -------------------------------------------------
    def _pop(self, n: int = 1) -> list[Value]:
        if n <= 0:
            return []
        if len(self.stack) < n:
            raise VMError(f"stack underflow: need {n}, have {len(self.stack)}")
        out = self.stack[-n:]
        del self.stack[-n:]
        return out

    def _pop_int(self) -> int:
        if not self.stack or self.stack[-1].typ != "int":
            raise VMError("stack or type check failed")
        return _i32(int(self.stack.pop().value or 0))

    def _pop_float(self) -> float:
        if not self.stack or self.stack[-1].typ != "float":
            raise VMError("stack or type check failed")
        return float(self.stack.pop().value or 0)

    def _pop_str(self) -> str:
        if not self.stack or self.stack[-1].typ != "string":
            raise VMError("stack or type check failed")
        return self.stack.pop().value

    def _peek(self, n: int = 1) -> list[Value]:
        if len(self.stack) < n:
            raise VMError(f"stack underflow: need {n}, have {len(self.stack)}")
        return self.stack[-n:]

    def _push_default(self, typ: str) -> None:
        self.stack.extend(default_cells(typ))

    def _target(self, ins: ncs.Instruction) -> int:
        delta = ins.ops[0][1]
        off = ins.offset + delta
        idx = self.by_off.get(off)
        if idx is None:
            raise VMError(f"jump target 0x{off:X} is not an instruction")
        return idx

    # ---- action mediation ---------------------------------------------
    def _action(self, ins: ncs.Instruction) -> None:
        aid, argc = ins.action
        spec = self.specs.get(aid)
        if spec is None:
            raise VMError(f"unknown action id {aid}")
        params = spec["params"]
        args: list[list[Value]] = []
        # Handlers pop what they declare; a cell pushed beyond that stays (005473e0).
        for k in range(min(argc, len(params))):
            typ = params[k]["type"]
            if typ == "action":
                # Functors are created by STORESTATE and are not stack cells.
                args.append([])
                self.pending_functor = False
                continue
            width = STRUCT_SLOTS.get(typ, 1)
            cells = self._pop(width)
            wanted = {"int": "int", "float": "float", "vector": "float", "string": "string", "object": "object"}.get(typ)
            if wanted is not None and any(cell.typ != wanted for cell in cells):
                self.notes["action_arg_type"] += 1
                raise VMError("stack or type check failed")
            args.append(cells)
        if argc < len(params):
            self.notes["action_missing_args"] += 1
        elif argc > len(params):
            self.notes["action_extra_args"] += 1
        self.action_counts[aid] += 1
        if self.runtime is not None:
            cells = self.runtime.call(aid, args, self)
            if cells:
                self.stack.extend(cells)
            return
        if aid in self.blocking_actions:
            self.status = "blocked"
            self.blocked_action = aid
            return
        ret = spec["return"]
        if ret != "void":
            self._push_default(ret)

    # ---- execution -----------------------------------------------------
    def _step(self, ins: ncs.Instruction) -> int | None:
        name = ins.name
        q = ins.qualifier
        next_ip = self.ip + 1

        if name == "NOP":
            raise VMError("unknown opcode")  # Past the dispatch bound at 005d2c45.
        if name == "STORESTATEALL":
            return next_ip
        if name == "STORESTATE":
            self.pending_functor = True
            return next_ip
        if name == "CPTOPSP":
            off, size = ins.ops[0][1], ins.ops[1][1]
            idx, count = len(self.stack) + _cell_offset(off), _cell_count(size)
            if count <= 0:
                return next_ip
            if idx < 0 or idx + count > len(self.stack):
                raise VMError(f"CPTOPSP outside frame ({off},{size}) sp={len(self.stack)}")
            self.stack.extend(self.stack[idx:idx + count])
            return next_ip
        if name == "CPDOWNSP":
            off, size = ins.ops[0][1], ins.ops[1][1]
            idx, count = len(self.stack) + _cell_offset(off), _cell_count(size)
            if count <= 0:
                return next_ip
            if len(self.stack) < count or idx < 0 or idx + count > len(self.stack):
                raise VMError(f"CPDOWNSP outside frame ({off},{size}) sp={len(self.stack)}")
            self.stack[idx:idx + count] = self.stack[-count:]
            return next_ip
        if name == "CPTOPBP":
            off, size = ins.ops[0][1], ins.ops[1][1]
            idx, count = self.bp + _cell_offset(off), _cell_count(size)
            if count <= 0:
                return next_ip
            if idx < 0 or idx + count > len(self.stack):
                raise VMError(f"CPTOPBP outside globals ({off},{size}) bp={self.bp}")
            self.stack.extend(self.stack[idx:idx + count])
            return next_ip
        if name == "CPDOWNBP":
            off, size = ins.ops[0][1], ins.ops[1][1]
            idx, count = self.bp + _cell_offset(off), _cell_count(size)
            if count <= 0:
                return next_ip
            if len(self.stack) < count or idx < 0 or idx + count > len(self.stack):
                raise VMError(f"CPDOWNBP outside globals ({off},{size}) bp={self.bp}")
            self.stack[idx:idx + count] = self.stack[-count:]
            return next_ip
        if name == "RSADD":
            if q in (0x03, 0x04, 0x05, 0x06):
                self._push_default(TYPE_BY_QUAL[q])
            elif 0x10 <= q <= 0x19:
                self._push_default("effect")
            else:
                raise VMError("bad operand form")
            return next_ip
        if name == "CONST":
            if q == 0x03:
                self.stack.append(Value("int", ins.ops[0][1]))
            elif q == 0x04:
                self.stack.append(Value("float", ins.ops[0][1]))
            elif q == 0x05:
                # Built as a C string, so a NUL ends it (005e5680).
                self.stack.append(Value("string", ins.ops[0][1].split(b"\0")[0].decode("latin-1")))
            elif q == 0x06:
                self.stack.append(Value("object", ins.ops[0][1]))
            else:
                raise VMError(f"bad CONST type 0x{q:02X}")
            return next_ip
        if name == "ACTION":
            self._action(ins)
            return None if self.status == "blocked" else next_ip
        if name == "SAVEBP":
            self.stack.append(Value("int", self.bp))
            # BP points at the pushed saved-BP cell; globals are BP-1, BP-2, ...
            # (verified against the live engine: SP=191, BP=190 at the first action).
            self.bp = len(self.stack) - 1
            return next_ip
        if name == "RESTOREBP":
            saved = self._pop(1)[0]
            self.bp = saved.as_int()
            return next_ip
        if name == "MOVSP":
            off = ins.ops[0][1]
            # Only ever shrinks: 005d21f0 ignores a target at or above SP.
            if off < 0:
                n = -_cell_offset(off)
                if n:
                    self._pop(n)
            return next_ip
        if name in ("INCSP", "DECSP"):
            off = ins.ops[0][1]
            if q != 0x03:
                return next_ip
            idx = len(self.stack) + _cell_offset(off)
            if idx < 0 or idx >= len(self.stack):
                raise VMError(f"{name} outside stack ({off}) sp={len(self.stack)}")
            self._incdec(self.stack[idx], 1 if name == "INCSP" else -1)
            return next_ip
        if name in ("INCBP", "DECBP"):
            off = ins.ops[0][1]
            if q != 0x03:
                return next_ip
            idx = self.bp + _cell_offset(off)
            if idx < 0 or idx >= len(self.stack):
                raise VMError(f"{name} outside globals ({off}) bp={self.bp}")
            self._incdec(self.stack[idx], 1 if name == "INCBP" else -1)
            return next_ip
        if name == "DESTRUCT":
            size, offset, keep = (_cell_count(v) for _, v in ins.ops)
            if size < 0 or offset < 0 or keep < 0 or offset + keep > size:
                raise VMError(f"DESTRUCT invalid ({size},{offset},{keep})")
            if size:
                start = len(self.stack) - size
                if start < 0:
                    raise VMError(f"DESTRUCT underflow size={size} sp={len(self.stack)}")
                retained = self.stack[start + offset:start + offset + keep]
                self.stack[start:] = retained
            return next_ip
        if name == "JMP":
            return self._target(ins)
        if name == "JSR":
            # retail 005d17f0: a 128-entry return stack that fails once it holds 128, counting
            # the run's own sentinel (005d4270). 8 (005d2260) limits nested runs.
            if len(self.call_stack) + 2 >= 128:
                raise VMError("recursion limit 128 exceeded")
            self.call_stack.append(next_ip)
            return self._target(ins)
        if name == "RETN":
            if self.call_stack:
                return self.call_stack.pop()
            self.status = "returned"
            return None
        if name in ("JZ", "JNZ"):
            cond = self._pop_int() != 0
            take = cond if name == "JNZ" else not cond
            return self._target(ins) if take else next_ip
        if name in ("LOGAND", "LOGOR"):
            if q != 0x20:
                raise VMError("bad operand form")
            b = self._pop_int() != 0
            a = self._pop_int() != 0
            val = (a and b) if name == "LOGAND" else (a or b)
            self.stack.append(Value("int", 1 if val else 0))
            return next_ip
        if name in ("BOOLAND", "INCOR", "EXCOR"):
            if q != 0x20:
                raise VMError("bad operand form")
            b = self._pop_int()
            a = self._pop_int()
            val = a & b if name == "BOOLAND" else a | b if name == "INCOR" else a ^ b
            self.stack.append(Value("int", _i32(val)))
            return next_ip
        if name in ("SHLEFT", "SHRIGHT", "USHRIGHT"):
            if q != 0x20:
                raise VMError("bad operand form")
            b = self._pop_int() & 31
            a = self._pop_int()
            if name == "SHLEFT":
                val = _i32(a << b)
            elif name == "USHRIGHT":
                val = a >> b  # `sar`, not `shr` (005d36da).
            elif a < 0:
                val = _i32(-(_i32(-a) >> b))  # Rounds toward zero (005d2bd0).
            else:
                val = a >> b
            self.stack.append(Value("int", val))
            return next_ip
        if name in ("ADD", "SUB", "MUL", "DIV", "MOD"):
            if name == "MOD" and q != 0x20:
                raise VMError("bad operand form")
            if q == 0x20:
                b = self._pop_int()
                a = self._pop_int()
                if name == "ADD":
                    val = a + b
                elif name == "SUB":
                    val = a - b
                elif name == "MUL":
                    val = a * b
                else:
                    if b == 0:
                        raise VMError("divide by zero")
                    if a == -0x80000000 and b == -1:
                        raise VMError("integer divide overflow")
                    quotient = abs(a) // abs(b)
                    quotient = -quotient if (a < 0) != (b < 0) else quotient
                    val = quotient if name == "DIV" else a - quotient * b
                self.stack.append(Value("int", _i32(val)))
            elif q in (0x21, 0x25, 0x26):
                b = _f32(float(self._pop_int())) if q == 0x26 else self._pop_float()
                a = _f32(float(self._pop_int())) if q == 0x25 else self._pop_float()
                if name == "ADD":
                    val = a + b
                elif name == "SUB":
                    val = a - b
                elif name == "MUL":
                    val = a * b
                else:
                    if b == 0.0:
                        raise VMError("divide by zero")
                    val = a / b
                self.stack.append(Value("float", _f32(val)))
            elif q == 0x23:
                b = self._pop_str()
                a = self._pop_str()
                if name == "ADD":
                    self.stack.append(Value("string", a + b))
            elif q == 0x3A:
                bz, by, bx = self._pop_float(), self._pop_float(), self._pop_float()
                az, ay, ax = self._pop_float(), self._pop_float(), self._pop_float()
                if name == "ADD":
                    self.stack.extend(Value("float", _f32(v)) for v in (ax + bx, ay + by, az + bz))
                elif name == "SUB":
                    self.stack.extend(Value("float", _f32(v)) for v in (ax - bx, ay - by, az - bz))
            elif q in (0x3B, 0x3C):
                if q == 0x3B:
                    scalar = self._pop_float()
                    vz, vy, vx = self._pop_float(), self._pop_float(), self._pop_float()
                else:
                    vz, vy, vx = self._pop_float(), self._pop_float(), self._pop_float()
                    scalar = self._pop_float()
                if name == "MUL":
                    self.stack.extend(Value("float", _f32(scalar * v)) for v in (vx, vy, vz))
                elif name == "DIV":
                    if scalar == 0.0:
                        raise VMError("divide by zero")
                    inverse = _f32(1.0 / scalar)
                    self.stack.extend(Value("float", _f32(inverse * v)) for v in (vx, vy, vz))
            else:
                raise VMError("bad operand form")
            return next_ip
        if name in ("EQ", "EQUAL", "NEQ", "NEQUAL"):
            if not (0x20 <= q <= 0x24 or 0x30 <= q <= 0x39):
                raise VMError("bad operand form")
            n = _cell_count(ins.ops[0][1]) if q == 0x24 and ins.ops else 1
            if len(self.stack) < n * 2:
                raise VMError("stack or type check failed")
            equal = True
            for i in range(1, n + 1):
                right = self.stack[-i]
                left = self.stack[-i - n]
                if right.typ == "string":
                    # 005e6450: ASCII case-insensitive.
                    same = left.typ == "string" and left.as_str().lower() == right.as_str().lower()
                elif right.typ == "engine":
                    same = left.value == right.value
                else:
                    same = _bits(left) == _bits(right)
                equal = equal and same
            if n > 0:
                del self.stack[-n * 2:]
            val = equal if name in ("EQ", "EQUAL") else not equal
            self.stack.append(Value("int", 1 if val else 0))
            return next_ip
        if name in ("GEQ", "GT", "LT", "LEQ"):
            if q == 0x20:
                y = self._pop_int()
                x = self._pop_int()
            elif q == 0x21:
                y = self._pop_float()
                x = self._pop_float()
            else:
                raise VMError("bad operand form")
            if name == "GEQ":
                val = x >= y
            elif name == "GT":
                val = x > y
            elif name == "LT":
                val = x < y
            else:
                val = x <= y
            self.stack.append(Value("int", 1 if val else 0))
            return next_ip
        if name == "NEG":
            if q == 0x03:
                self.stack.append(Value("int", _i32(-self._pop_int())))
            elif q == 0x04:
                self.stack.append(Value("float", -self._pop_float()))
            else:
                raise VMError("bad operand form")
            return next_ip
        if name == "COMP":
            if q != 0x03:
                raise VMError("bad operand form")
            self.stack.append(Value("int", ~self._pop_int()))
            return next_ip
        if name == "NOT":
            if q != 0x03:
                raise VMError("bad operand form")
            self.stack.append(Value("int", 1 if self._pop_int() == 0 else 0))
            return next_ip
        raise VMError(f"unsupported opcode 0x{ins.opcode:02X} {name}")

    @staticmethod
    def _incdec(v: Value, delta: int) -> None:
        # INC/DEC only touch an int cell (005d16c0).
        if v.typ == "int":
            v.value = _i32(v.as_int() + delta)

    def run(self) -> dict:
        try:
            while self.ip is not None:
                if self.ip >= len(self.ins):
                    raise VMError("ran past the end of the code")  # retail -0x6b
                if self.steps >= self.budget:
                    self.status = "budget"
                    break
                ins = self.ins[self.ip]
                self.current_ins = ins
                self.steps += 1
                self.ip = self._step(ins)
                if len(self.stack) > self.max_sp:
                    self.max_sp = len(self.stack)
        except VMError as exc:
            self.status = "error"
            self.error = str(exc)
            self.error_ip = self.current_ins.offset if self.current_ins else None
        except Exception as exc:  # defensive: one bad script must not stop the corpus
            self.status = "error"
            self.error = f"{type(exc).__name__}: {exc}"
            self.error_ip = self.current_ins.offset if self.current_ins else None
        if self.status is None:
            self.status = "returned"
        # A failed run takes nothing back but its stack (005d4270).
        if self.status in ("error", "budget"):
            self.stack.clear()
        return self.result()

    def result(self) -> dict:
        top = None
        if self.stack:
            v = self.stack[-1]
            top = {"type": v.typ, "value": v.value}
        out = {
            "status": self.status,
            "steps": self.steps,
            "max_sp": self.max_sp,
            "actions": dict(sorted(self.action_counts.items())),
            "action_calls": sum(self.action_counts.values()),
            "notes": dict(sorted(self.notes.items())),
            "error": self.error,
            "error_ip": f"0x{self.error_ip:X}" if self.error_ip is not None else None,
            "blocked_action": self.blocked_action,
            "top": top,
        }
        if self.runtime is not None:
            out["runtime"] = self.runtime.summary()
        return out


def validate_control_flow(instructions: list[ncs.Instruction]) -> list[dict]:
    """Return jump/STORESTATE targets that do not land on an instruction."""
    offsets = {i.offset for i in instructions}
    bad = []
    for ins in instructions:
        targets = []
        if ins.name in ("JMP", "JSR", "JZ", "JNZ"):
            targets.append(("jump", ins.offset + ins.ops[0][1]))
        elif ins.name == "STORESTATE" and ins.qualifier:
            targets.append(("storestate", ins.offset + ins.qualifier))
        for kind, target in targets:
            if target not in offsets:
                bad.append({"kind": kind, "at": f"0x{ins.offset:X}",
                            "target": f"0x{target:X}"})
    return bad


def run_file(path: str, specs: dict, budget: int = RETAIL_BUDGET,
             runtime_factory=None) -> dict:
    data = Path(path).read_bytes()
    try:
        instructions = ncs.decode(data)
    except Exception as exc:
        return {"status": "decode_error", "error": str(exc), "steps": 0,
                "instruction_count": 0, "invalid_jump_targets": []}
    runtime = runtime_factory() if runtime_factory is not None else None
    result = VM(instructions, specs, budget=budget, runtime=runtime).run()
    result["instruction_count"] = len(instructions)
    result["invalid_jump_targets"] = validate_control_flow(instructions)
    return result


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--one")
    ap.add_argument("--corpus")
    ap.add_argument("--budget", type=int, default=200_000)
    ap.add_argument("--runtime", action="store_true",
                    help="use the stateful action runtime instead of typed defaults")
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    specs = json.loads(Path(args.specs).read_text(encoding="utf-8"))
    factory = None
    if args.runtime:
        from vm.actions_runtime import ActionRuntime
        factory = lambda: ActionRuntime(specs)  # noqa: E731
    if args.one:
        print(json.dumps(run_file(args.one, specs, args.budget, factory),
                         indent=2, sort_keys=True))
        return 0
    if not args.corpus:
        ap.error("one of --one or --corpus is required")
    rows = []
    for path in sorted(glob.glob(os.path.join(args.corpus, "*.ncs"))):
        r = run_file(path, specs, args.budget, factory)
        r["file"] = os.path.basename(path)
        rows.append(r)
    counts = Counter(r["status"] for r in rows)
    summary = {"count": len(rows), "status_counts": dict(sorted(counts.items())),
               "action_calls": sum(r.get("action_calls", 0) for r in rows),
               "files": rows}
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("count", "status_counts", "action_calls")},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
