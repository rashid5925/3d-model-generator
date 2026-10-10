// OS integration: where the executable lives, where output goes by default,
// stdio setup for the MCP transport, and registering the server with MCP
// clients (Claude Code, Claude Desktop, Cursor, Windsurf, VS Code Copilot, Copilot
// CLI, OpenCode, Codex).
#pragma once

#include <string>
#include <vector>

namespace sf {

std::string executablePath();
std::string homeDirectory();
// Output directory used when --workdir is not given: the current directory if
// it is a normal writable folder, otherwise ~/Documents/shapeforge (or
// ~/shapeforge on Linux). MCP clients often launch servers with cwd = "/".
std::string defaultWorkdir();
// Where `install` points clients by default: ~/Documents/shapeforge
// (~/shapeforge on Linux).
std::string recommendedWorkdir();
// Binary stdio on Windows so JSON-RPC lines are not rewritten as CRLF.
void configureStdio();

enum class ClientFormat { ClaudeCli, McpServersJson, VscodeJson, CopilotCliJson, OpencodeJson, CodexToml };

struct ClientInfo {
    std::string id;           // claude-code, claude-desktop, cursor, windsurf, vscode, copilot-cli, opencode, codex
    std::string displayName;
    std::string configPath;   // config file (empty for claude-code, which has a CLI)
    ClientFormat format;
    std::string appDir;       // exists when the client is installed
};
std::vector<ClientInfo> knownClients();
// "configured", "not configured" or "not installed".
std::string clientStatus(const ClientInfo& client);

// Prints the JSON / commands needed to register this executable with each client.
std::string clientConfigInstructions(const std::string& workdir);
// Registers the server with a client. Returns a human-readable report; throws on failure.
std::string installIntoClient(const std::string& clientId, const std::string& workdir);
std::string uninstallFromClient(const std::string& clientId);

}  // namespace sf
