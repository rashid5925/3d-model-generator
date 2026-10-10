#!/usr/bin/env node
// Assembles the npm packages from release binaries.
//   node packaging/npm/build-packages.mjs <version> <dist-dir> <out-dir>
// <dist-dir> must contain: shapeforge-macos-universal, shapeforge-linux-x64,
// shapeforge-linux-arm64, shapeforge-windows-x64.exe
// Produces <out-dir>/<package>/ folders ready for `npm publish`.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const [version, distDir, outDir] = process.argv.slice(2);
if (!version || !distDir || !outDir) {
  console.error("usage: build-packages.mjs <version> <dist-dir> <out-dir>");
  process.exit(1);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../..");

const platforms = [
  { name: "shapeforge-darwin", os: ["darwin"], cpu: ["arm64", "x64"], src: "shapeforge-macos-universal", exe: "shapeforge" },
  { name: "shapeforge-linux-x64", os: ["linux"], cpu: ["x64"], src: "shapeforge-linux-x64", exe: "shapeforge" },
  { name: "shapeforge-linux-arm64", os: ["linux"], cpu: ["arm64"], src: "shapeforge-linux-arm64", exe: "shapeforge" },
  { name: "@rashid5925/shapeforge-win32-x64", os: ["win32"], cpu: ["x64", "arm64"], src: "shapeforge-windows-x64.exe", exe: "shapeforge.exe" },
];

const main = JSON.parse(fs.readFileSync(path.join(here, "shapeforge/package.json"), "utf8"));
const repo = main.repository;
fs.rmSync(outDir, { recursive: true, force: true });

for (const p of platforms) {
  const dir = path.join(outDir, p.name);
  fs.mkdirSync(path.join(dir, "bin"), { recursive: true });
  const dest = path.join(dir, "bin", p.exe);
  fs.copyFileSync(path.join(distDir, p.src), dest);
  fs.chmodSync(dest, 0o755);
  fs.writeFileSync(
    path.join(dir, "package.json"),
    JSON.stringify(
      {
        name: p.name,
        version,
        description: `Prebuilt shapeforge binary for ${p.os[0]} (${p.cpu.join(", ")})`,
        license: main.license,
        repository: repo,
        os: p.os,
        cpu: p.cpu,
        files: ["bin/"],
        preferUnplugged: true,
      },
      null,
      2,
    ) + "\n",
  );
  fs.writeFileSync(path.join(dir, "README.md"), `Platform binary for the [shapeforge](https://www.npmjs.com/package/shapeforge) package. Install \`shapeforge\` instead.\n`);
}

const mainDir = path.join(outDir, "shapeforge");
fs.cpSync(path.join(here, "shapeforge"), mainDir, { recursive: true });
main.version = version;
for (const p of platforms) main.optionalDependencies[p.name] = version;
fs.writeFileSync(path.join(mainDir, "package.json"), JSON.stringify(main, null, 2) + "\n");
fs.chmodSync(path.join(mainDir, "bin", "shapeforge.js"), 0o755);
fs.copyFileSync(path.join(root, "packaging/npm/README.md"), path.join(mainDir, "README.md"));
console.log(`npm packages for ${version} written to ${outDir}`);
