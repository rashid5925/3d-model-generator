# shapeforge

**Let your AI assistant build 3D models.** shapeforge is an MCP server: your
agent builds a model from named parts, renders it to see it, measures it, fixes
it, and exports **OBJ**. It's a native binary that runs locally, with no
Python, Blender or GPU SDK needed.

## Set up

Register it with your agent in one command:

```bash
npx -y shapeforge install claude-code
```

Agents: `claude-code`, `claude-desktop`, `cursor`, `windsurf`, `vscode` (GitHub
Copilot), `copilot-cli`, `opencode`, `codex`, or `all`. Restart the agent
afterwards.

Or add it by hand. Most agents accept:

```json
{ "mcpServers": { "shapeforge": { "command": "npx", "args": ["-y", "shapeforge"] } } }
```

`npx -y shapeforge config` prints the exact snippet for every agent.

## Use

Ask your agent something like:

> Model a wooden dining chair, 45 cm seat height, and export it as OBJ.

Models are saved to `~/Documents/shapeforge` (Linux: `~/shapeforge`).
`npx -y shapeforge doctor` checks your setup.

More: https://github.com/rashid5925/3d-model-generator
