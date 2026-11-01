"""Minimal LYT (#MAXLAYOUT ASCII) parser: room origins and door hooks.

Layout verified on the shipped corpus (docs/recon/04 §3): room lines are
``<room> <x> <y> <z>`` under ``roomcount``; door hooks are
``<room> <node> <attach> <x> <y> <z> <qw> <qx> <qy> <qz>`` under
``doorhookcount`` (quaternion WXYZ).
"""

from __future__ import annotations


def parse_lyt_text(text: str) -> tuple[list[dict], list[dict]]:
    rooms: list[dict] = []
    hooks: list[dict] = []
    mode = None
    for ln in text.splitlines():
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        low = s.lower()
        if low.startswith("roomcount"):
            mode = "room"
        elif low.startswith("doorhookcount"):
            mode = "door"
        elif low.startswith(("trackcount", "obstaclecount", "donelayout",
                             "beginlayout")):
            mode = None
        elif mode == "room":
            p = s.split()
            rooms.append({"room": p[0], "x": float(p[1]), "y": float(p[2]),
                          "z": float(p[3])})
        elif mode == "door":
            p = s.split()
            hooks.append({"room": p[0], "node": p[1],
                          "x": float(p[3]), "y": float(p[4]), "z": float(p[5]),
                          "quat": tuple(float(v) for v in p[6:10])})
    return rooms, hooks


def parse_lyt(data: bytes) -> tuple[list[dict], list[dict]]:
    return parse_lyt_text(data.decode("latin-1"))
