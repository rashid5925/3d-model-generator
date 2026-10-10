# Releasing

Releases are built and published by `.github/workflows/release.yml` when a
`v*` tag is pushed.

## Published artifacts

| Artifact | Platform |
|---|---|
| `shapeforge-macos-universal` | macOS 11+, Apple Silicon and Intel; MoltenVK linked statically |
| `shapeforge-linux-x64`, `shapeforge-linux-arm64` | glibc 2.28+ (built in `manylinux_2_28`) |
| `shapeforge-windows-x64.exe` | Windows 10+, static MSVC runtime |
| `shapeforge.mcpb` | Claude Desktop extension bundle containing all platforms |
| npm: `shapeforge` + `shapeforge-{darwin,linux-x64,linux-arm64,win32-x64}` | Launcher plus per-platform binaries |
| MCP Registry: `io.github.rashid5925/shapeforge` | Registry entry pointing at the npm package |
| `SHA256SUMS` | Checksums, verified by the install scripts |

## Repository setup (once)

| Secret | Purpose |
|---|---|
| `NPM_TOKEN` | npm automation/granular token with publish rights. Also enables the MCP Registry step. |
| `MACOS_CERT_P12`, `MACOS_CERT_PASSWORD`, `MACOS_SIGN_IDENTITY`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | Optional: Developer ID signing and notarization |

The MCP Registry uses GitHub OIDC (`mcp-publisher login github-oidc`), so it
needs no secret.

To publish the landing page, set **Settings → Pages → Source** to
**GitHub Actions**. `pages.yml` then deploys `site/` on every change.

## Cutting a release

1. Make sure CI is green on `main`.
2. Tag and push:

   ```bash
   git tag v0.2.0
   ```

   ```bash
   git push origin v0.2.0
   ```

3. The workflow builds and tests on every platform, then publishes in order:
   npm, MCP Registry, GitHub Release.
4. Smoke-test the published build:

   ```bash
   npx -y shapeforge@latest doctor
   ```

A dry run (artifacts only, nothing published) is available from the Actions
tab: **release → Run workflow**.

## Code signing

- **macOS:** handled by the workflow when the Apple secrets are present
  (hardened runtime + notarization).
- **Windows:** not configured yet. Options are SignPath Foundation (free for
  OSS), Azure Artifact Signing, or an OV certificate on a cloud HSM. Add the
  signing step to the `windows` job before staging the executable, and always
  timestamp signatures.

## Homebrew (optional)

`packaging/homebrew/shapeforge.rb` is a formula template for a
`rashid5925/homebrew-tap` repository. Update the version and checksums from
`SHA256SUMS` on each release.
