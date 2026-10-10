#!/usr/bin/env python3
"""Builds a small robot purely through MCP tool calls - the same JSON an LLM sends.

Usage: python3 examples/robot_demo.py path/to/shapeforge [output-dir]
Writes robot.png, robot.json and robot.obj/.mtl into the output dir.
"""
import base64
import json
import subprocess
import sys

exe = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else "out"
proc = subprocess.Popen([exe, "mcp", "--workdir", outdir], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
next_id = 0


def rpc(method, params):
    global next_id
    next_id += 1
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params}) + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())["result"]


def tool(name, **args):
    res = rpc("tools/call", {"name": name, "arguments": args})
    text = "\n".join(c["text"] for c in res["content"] if c["type"] == "text")
    print(f"--> {name}: {'ERROR ' + text if res.get('isError') else 'ok'}")
    if res.get("isError"):
        sys.exit(1)
    return res


rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "demo", "version": "1"}})

# 1. Parts, roughly placed. Exact contact is left to place_on.
tool("add_objects", objects=[
    {"name": "robot", "shape": "group"},
    {"name": "torso", "parent": "robot", "shape": "box", "size": [0.5, 0.6, 0.3], "rounding": 0.05,
     "position": [0, 0.75, 0], "color": "#4f7cac"},
    {"name": "head", "parent": "robot", "shape": "box", "size": [0.34, 0.26, 0.28], "rounding": 0.06,
     "position": [0, 1.3, 0], "color": "#c9d6df"},
    {"name": "eye_left", "parent": "head", "shape": "cylinder", "radius": 0.035, "height": 0.02,
     "rotation": [90, 0, 0], "position": [-0.08, 0.02, 0.15], "color": "#ffd166"},
    {"name": "antenna", "parent": "robot", "shape": "cylinder", "radius": 0.01, "height": 0.15,
     "position": [0, 2, 0], "color": "steel"},
    {"name": "antenna_tip", "parent": "antenna", "shape": "sphere", "radius": 0.025, "position": [0, 0.085, 0],
     "color": "red"},
    {"name": "leg_left", "parent": "robot", "shape": "csg", "op": "union", "smooth": 0.03,
     "position": [-0.13, 0.25, 0], "color": "#3d5a80",
     "children": [{"shape": "cylinder", "radius": 0.06, "height": 0.4, "position": [0, 0.05, 0]},
                  {"shape": "box", "size": [0.14, 0.06, 0.22], "position": [0, -0.17, 0.04]}]},
    {"name": "arm_left", "parent": "robot", "shape": "capsule", "radius": 0.05, "height": 0.5,
     "position": [-0.33, 0.8, 0], "rotation": [0, 0, -12], "color": "#3d5a80"},
    {"name": "button", "parent": "robot", "shape": "cylinder", "radius": 0.03, "height": 0.02,
     "rotation": [90, 0, 0], "position": [0, 0.9, 0.3], "color": "#ee6c4d"},
    {"name": "wheel_base", "shape": "torus", "radius": 0.5, "tube": 0.03, "position": [0, 0.5, 0], "color": "gold"},
    {"name": "spoke", "parent": "wheel_base", "shape": "cylinder", "radius": 0.012, "height": 0.47,
     "rotation": [0, 0, 90], "position": [0.25, 0, 0], "color": "silver"},
])

# 2. Symmetry and repetition.
tool("mirror_object", object="eye_left")
tool("mirror_object", object="leg_left")
tool("mirror_object", object="arm_left")
tool("array_object", object="button", count=3, offset=[0, -0.12, 0])
tool("array_object", object="spoke", count=8, mode="radial", axis="y")

# 3. Exact placement instead of hand-computed coordinates.
tool("place_on", object="robot", target="ground")              # whole assembly onto the floor
tool("place_on", object="head", target="torso", center=True)   # head sits on the torso
tool("place_on", object="antenna", target="head", center=True)
tool("place_on", objects=["button", "button_2", "button_3"], target="torso", side="front")
tool("update_objects", updates=[{"name": "wheel_base", "position": [1.3, 0, 0], "rotation": [90, 0, 0]}])
tool("place_on", object="wheel_base", target="ground")

# 4. Look, measure, export.
res = tool("render", views=["front", "iso", "right", {"azimuth": -40, "elevation": 15}], labels=False)
png = next(c for c in res["content"] if c["type"] == "image")
with open(f"{outdir}/robot.png", "wb") as f:
    f.write(base64.b64decode(png["data"]))
print(json.dumps(json.loads(tool("inspect")["content"][0]["text"])["warnings"], indent=1))
tool("save_scene", path="robot.json")
tool("export_obj", path="robot.obj")
proc.stdin.close()
proc.wait()
