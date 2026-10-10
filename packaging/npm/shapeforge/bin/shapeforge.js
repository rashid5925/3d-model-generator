#!/usr/bin/env node
// Thin launcher: finds the prebuilt native binary that npm installed for this
// platform (an optionalDependency) and runs it with stdio passed straight
// through, so MCP's JSON-RPC stream is untouched.
"use strict";

const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const PACKAGES = {
  "darwin-arm64": "shapeforge-darwin",
  "darwin-x64": "shapeforge-darwin",
  "linux-x64": "shapeforge-linux-x64",
  "linux-arm64": "shapeforge-linux-arm64",
  "win32-x64": "@rashid5925/shapeforge-win32-x64",
  "win32-arm64": "@rashid5925/shapeforge-win32-x64", // x64 emulation on Windows on ARM
};

const key = `${process.platform}-${process.arch}`;
const pkg = PACKAGES[key];
if (!pkg) {
  console.error(`shapeforge: no prebuilt binary for ${key}. Build from source: https://github.com/rashid5925/3d-model-generator`);
  process.exit(1);
}

const exe = process.platform === "win32" ? "shapeforge.exe" : "shapeforge";
let binary;
try {
  binary = require.resolve(`${pkg}/bin/${exe}`);
} catch (e) {
  console.error(
    `shapeforge: the platform package "${pkg}" is missing. ` +
      "Reinstall without --no-optional / --omit=optional, or download a binary from GitHub Releases.",
  );
  process.exit(1);
}

try {
  fs.accessSync(binary, fs.constants.X_OK);
} catch {
  try {
    fs.chmodSync(binary, 0o755);
  } catch {}
}

// When run through npx the binary sits in a cache that may be cleaned up, so
// tell it to register "npx -y shapeforge" with MCP clients instead of its own
// path. For regular installs the binary's absolute path is stable and is used
// as-is (GUI apps often lack the shell's PATH, so a bare name is not enough).
const viaNpx = __dirname.includes(`${path.sep}_npx${path.sep}`);
const env = { ...process.env };
if (viaNpx && !env.SHAPEFORGE_LAUNCHER) env.SHAPEFORGE_LAUNCHER = "npx -y shapeforge";

const args = process.argv.slice(2);
if (args.length === 0) args.push("mcp"); // `npx -y shapeforge` == run the MCP server

const child = spawn(binary, args, { stdio: "inherit", env, windowsHide: true });
for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) {
  process.on(sig, () => child.kill(sig));
}
child.on("error", (err) => {
  console.error(`shapeforge: failed to start ${binary}: ${err.message}`);
  process.exit(1);
});
child.on("exit", (code, signal) => {
  if (signal) process.kill(process.pid, signal);
  else process.exit(code === null ? 1 : code);
});
