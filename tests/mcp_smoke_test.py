#!/usr/bin/env python3
"""End-to-end test of the MCP server over stdio, the way an LLM client uses it.

Usage: mcp_smoke_test.py <path-to-shapeforge> [--renderer cpu|vulkan|auto] [--keep DIR]
"""
import base64
import json
import os
import subprocess
import sys
import tempfile


class Client:
    def __init__(self, exe, renderer, workdir):
        self.proc = subprocess.Popen(
            [exe, "mcp", "--renderer", renderer, "--workdir", workdir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.next_id = 1

    def request(self, method, params=None):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.next_id += 1
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed the connection: " + self.proc.stderr.read())
        resp = json.loads(line)
        assert resp["id"] == msg["id"], resp
        return resp

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def call(self, tool, args):
        resp = self.request("tools/call", {"name": tool, "arguments": args})
        assert "result" in resp, resp
        return resp["result"]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def text_of(result):
    return "\n".join(c["text"] for c in result["content"] if c["type"] == "text")


def main():
    exe = sys.argv[1]
    renderer = "cpu"
    keep = None
    args = sys.argv[2:]
    if "--renderer" in args:
        renderer = args[args.index("--renderer") + 1]
    if "--keep" in args:
        keep = args[args.index("--keep") + 1]
    workdir = keep or tempfile.mkdtemp(prefix="shapeforge-test-")
    os.makedirs(workdir, exist_ok=True)

    c = Client(exe, renderer, workdir)
    init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "smoke-test", "version": "1"}})
    assert init["result"]["serverInfo"]["name"] == "shapeforge"
    assert "instructions" in init["result"]
    c.notify("notifications/initialized")

    tools = {t["name"] for t in c.request("tools/list")["result"]["tools"]}
    expected = {"add_objects", "update_objects", "delete_objects", "get_scene", "clear_scene", "render",
                "inspect", "export_obj", "save_scene", "load_scene", "undo", "place_on", "align_objects",
                "mirror_object", "array_object"}
    assert expected <= tools, tools

    # Build a small snowman.
    r = c.call("add_objects", {"objects": [
        {"name": "snowman", "shape": "group"},
        {"name": "base", "parent": "snowman", "shape": "sphere", "radius": 0.4, "position": [0, 0.4, 0], "color": "white"},
        {"name": "body", "parent": "snowman", "shape": "sphere", "radius": 0.3, "position": [0, 1.0, 0], "color": "white"},
        {"name": "head", "parent": "snowman", "shape": "sphere", "radius": 0.2, "position": [0, 1.45, 0], "color": "white"},
        {"name": "nose", "parent": "head", "shape": "cone", "radius": 0.04, "height": 0.2,
         "rotation": [90, 0, 0], "position": [0, 0, 0.27], "color": "orange"},
        {"name": "hat", "parent": "head", "shape": "csg", "op": "union", "color": "black", "position": [0, 0.17, 0],
         "children": [{"shape": "cylinder", "radius": 0.22, "height": 0.02},
                      {"shape": "cylinder", "radius": 0.13, "height": 0.25, "position": [0, 0.125, 0]}]},
    ]})
    assert not r.get("isError"), text_of(r)
    report = json.loads(text_of(r).split("\n\nNext:")[0])
    assert len(report["added_or_replaced"]) == 6

    # copy_of + typo warning
    r = c.call("add_objects", {"objects": [
        {"name": "eye_l", "parent": "head", "shape": "sphere", "radius": 0.025, "position": [-0.07, 0.05, 0.18], "colour": "black"},
        {"name": "eye_r", "copy_of": "eye_l", "position": [0.07, 0.05, 0.18]}]})
    assert not r.get("isError"), text_of(r)
    assert "did you mean 'color'" in text_of(r), text_of(r)

    # Errors are reported as tool errors, and the batch is atomic.
    r = c.call("add_objects", {"objects": [
        {"name": "ok", "shape": "box"}, {"name": "bad", "shape": "pyramid"}]})
    assert r.get("isError") and "unknown shape 'pyramid'" in text_of(r), r
    scene = text_of(c.call("get_scene", {}))
    assert "- ok" not in scene

    # Render returns an image.
    r = c.call("render", {"views": ["front", "iso", {"azimuth": 120, "elevation": 10}], "size": 256,
                          "labels": True, "save_path": "renders/snowman.png"})
    assert not r.get("isError"), text_of(r)
    images = [x for x in r["content"] if x["type"] == "image"]
    assert len(images) == 1 and images[0]["mimeType"] == "image/png"
    png = base64.b64decode(images[0]["data"])
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert os.path.exists(os.path.join(workdir, "renders", "snowman.png"))

    # Inspect: the hat must touch the head, nothing floats.
    r = c.call("inspect", {"names": ["head", "hat"]})
    rep = json.loads(text_of(r))
    assert rep["relations"][0]["status"] in ("touching", "overlapping"), rep
    assert not any("float" in w for w in rep["warnings"]), rep["warnings"]

    # Update + undo.
    c.call("update_objects", {"updates": [{"name": "snowman", "translate": [1, 0, 0]}]})
    rep = json.loads(text_of(c.call("inspect", {"names": ["base"]})))
    assert abs(rep["objects"][0]["bounds"]["center"][0] - 1.0) < 1e-3
    c.call("undo", {})
    rep = json.loads(text_of(c.call("inspect", {"names": ["base"]})))
    assert abs(rep["objects"][0]["bounds"]["center"][0]) < 1e-3

    # Placement helpers.
    c.call("add_objects", {"objects": [
        {"name": "arm_left", "parent": "body", "shape": "capsule", "radius": 0.05, "height": 0.4,
         "position": [-0.35, 0.05, 0], "rotation": [0, 0, -60], "color": "brown"}]})
    r = c.call("mirror_object", {"object": "arm_left"})
    assert not r.get("isError") and "arm_right" in text_of(r), text_of(r)
    rep = json.loads(text_of(c.call("inspect", {"names": ["arm_left", "arm_right"]})))
    l, rr = rep["objects"][0]["bounds"], rep["objects"][1]["bounds"]
    assert abs(l["min"][0] + rr["max"][0]) < 1e-3, (l, rr)
    c.call("add_objects", {"objects": [{"name": "button", "shape": "sphere", "radius": 0.03, "color": "black",
                                        "position": [0, 3, 0.25]}]})
    r = c.call("place_on", {"object": "button", "target": "body", "side": "front", "at": [0, 0.05]})
    assert not r.get("isError"), text_of(r)
    rep = json.loads(text_of(c.call("inspect", {"names": ["button", "body"]})))
    assert rep["relations"][0]["status"] in ("touching", "overlapping"), rep
    r = c.call("array_object", {"object": "button", "count": 3, "offset": [0, -0.12, 0]})
    assert not r.get("isError") and "button_3" in text_of(r), text_of(r)
    r = c.call("align_objects", {"objects": ["button", "button_2", "button_3"], "axis": "x", "to": 0})
    assert not r.get("isError"), text_of(r)
    r = c.call("render", {"focus": ["head"], "views": ["front"], "size": 256})
    assert not r.get("isError"), text_of(r)

    # Save / load round trip and OBJ export.
    assert not c.call("save_scene", {"path": "snowman.json"}).get("isError")
    c.call("clear_scene", {})
    r = c.call("load_scene", {"path": "snowman.json"})
    assert not r.get("isError"), text_of(r)
    r = c.call("export_obj", {"path": "export/snowman.obj"})
    assert not r.get("isError"), text_of(r)
    info = json.loads(text_of(r))
    verify_obj(info["obj"], info["objects"])

    r = c.call("export_obj", {"path": "../escape.txt"})
    assert r.get("isError"), "non-.obj paths must be rejected"

    c.close()
    print(f"MCP smoke test passed ({renderer} renderer). Files in {workdir}")


def verify_obj(path, expected_objects):
    verts = normals = 0
    objects = 0
    with open(path) as f:
        lines = f.read().splitlines()
    assert lines[1].startswith("mtllib")
    for line in lines:
        if line.startswith("v "):
            verts += 1
        elif line.startswith("vn "):
            normals += 1
        elif line.startswith("o "):
            objects += 1
        elif line.startswith("f "):
            for corner in line.split()[1:]:
                vi, _, ni = corner.split("/")
                assert 1 <= int(vi) <= verts and 1 <= int(ni) <= normals, line
    assert objects == expected_objects, (objects, expected_objects)
    assert verts == normals and verts > 0
    assert os.path.exists(path[:-4] + ".mtl")


if __name__ == "__main__":
    main()
