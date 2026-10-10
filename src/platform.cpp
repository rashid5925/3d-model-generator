#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "json.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace sf {

namespace fs = std::filesystem;

std::string executablePath() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf).string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return "shapeforge";
    std::error_code ec;
    auto p = fs::canonical(buf.c_str(), ec);
    return ec ? std::string(buf.c_str()) : p.string();
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("shapeforge") : p.string();
#endif
}

std::string homeDirectory() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) return p;
#else
    if (const char* p = std::getenv("HOME")) return p;
#endif
    return fs::current_path().string();
}

namespace {

bool isWritableDir(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    fs::path probe = p / ".shapeforge-write-test";
    {
        std::ofstream f(probe);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

std::string appDataDir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("APPDATA")) return p;
    return (fs::path(homeDirectory()) / "AppData" / "Roaming").string();
#elif defined(__APPLE__)
    return (fs::path(homeDirectory()) / "Library" / "Application Support").string();
#else
    if (const char* p = std::getenv("XDG_CONFIG_HOME")) return p;
    return (fs::path(homeDirectory()) / ".config").string();
#endif
}

std::string quoteArg(const std::string& s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

// How MCP clients should launch us. When started through the npm wrapper,
// SHAPEFORGE_LAUNCHER (e.g. "npx -y shapeforge") is used instead of the binary's
// own path, which lives in a cache directory that may be cleaned up.
std::vector<std::string> launchCommand() {
    std::vector<std::string> parts;
    if (const char* l = std::getenv("SHAPEFORGE_LAUNCHER"); l && *l) {
        std::istringstream is(l);
        std::string w;
        while (is >> w) parts.push_back(w);
    }
    if (parts.empty()) parts.push_back(executablePath());
    return parts;
}

Json serverEntry(const std::string& workdir) {
    std::vector<std::string> cmd = launchCommand();
    Json args = Json::array();
    for (size_t i = 1; i < cmd.size(); ++i) args.push_back(cmd[i]);
    for (const char* a : {"mcp", "--workdir"}) args.push_back(a);
    args.push_back(workdir);
    return Json::object({{"command", cmd[0]}, {"args", std::move(args)}});
}

std::string launchCommandLine() {
    std::string out;
    std::vector<std::string> cmd = launchCommand();
    for (size_t i = 0; i < cmd.size(); ++i) out += (i ? " " : "") + (i == 0 ? quoteArg(cmd[i]) : cmd[i]);
    return out;
}

}  // namespace

std::string recommendedWorkdir() {
#if defined(__linux__)
    return (fs::path(homeDirectory()) / "shapeforge").string();
#else
    return (fs::path(homeDirectory()) / "Documents" / "shapeforge").string();
#endif
}

std::string defaultWorkdir() {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec && cwd != cwd.root_path() && cwd != fs::path(homeDirectory()) && isWritableDir(cwd)) return cwd.string();
    std::string dir = recommendedWorkdir();
    fs::create_directories(dir, ec);
    return dir;
}

void configureStdio() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

std::vector<ClientInfo> knownClients() {
    fs::path home = homeDirectory();
    fs::path xdg = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : home / ".config";
    fs::path codexHome = std::getenv("CODEX_HOME") ? fs::path(std::getenv("CODEX_HOME")) : home / ".codex";
    fs::path appData = appDataDir();
    return {
        {"claude-code", "Claude Code", "", ClientFormat::ClaudeCli, ""},
        {"claude-desktop", "Claude Desktop", (appData / "Claude" / "claude_desktop_config.json").string(),
         ClientFormat::McpServersJson, (appData / "Claude").string()},
        {"cursor", "Cursor", (home / ".cursor" / "mcp.json").string(), ClientFormat::McpServersJson,
         (home / ".cursor").string()},
        {"windsurf", "Windsurf", (home / ".codeium" / "windsurf" / "mcp_config.json").string(),
         ClientFormat::McpServersJson, (home / ".codeium" / "windsurf").string()},
        {"vscode", "VS Code (GitHub Copilot)", (appData / "Code" / "User" / "mcp.json").string(), ClientFormat::VscodeJson,
         (appData / "Code" / "User").string()},
        {"copilot-cli", "GitHub Copilot CLI", (home / ".copilot" / "mcp-config.json").string(),
         ClientFormat::CopilotCliJson, (home / ".copilot").string()},
        {"opencode", "OpenCode", (xdg / "opencode" / "opencode.json").string(), ClientFormat::OpencodeJson,
         (xdg / "opencode").string()},
        {"codex", "OpenAI Codex", (codexHome / "config.toml").string(), ClientFormat::CodexToml, codexHome.string()},
    };
}

namespace {

const char* kClientIds = "claude-code, claude-desktop, cursor, windsurf, vscode, copilot-cli, opencode, codex, copilot or all";

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (fs::exists(p)) fs::copy_file(p, p.string() + ".bak", fs::copy_options::overwrite_existing, ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out << text;
}

// The server entry each client expects, as JSON (TOML for Codex is built from the same parts).
Json clientEntry(ClientFormat f, const std::string& workdir) {
    Json base = serverEntry(workdir);
    switch (f) {
        case ClientFormat::VscodeJson:
            return Json::object({{"type", "stdio"}, {"command", base["command"]}, {"args", base["args"]}});
        case ClientFormat::CopilotCliJson:
            return Json::object(
                {{"type", "local"}, {"command", base["command"]}, {"args", base["args"]}, {"tools", Json::array({"*"})}});
        case ClientFormat::OpencodeJson: {
            Json cmd = Json::array({base["command"]});
            for (const auto& a : base["args"].asArray()) cmd.push_back(a);
            return Json::object({{"type", "local"}, {"command", std::move(cmd)}, {"enabled", true}});
        }
        default:
            return base;
    }
}

// Where in the JSON document the server lives: {"mcpServers": ...}, {"servers": ...} or {"mcp": ...}.
const char* serversKey(ClientFormat f) {
    if (f == ClientFormat::VscodeJson) return "servers";
    if (f == ClientFormat::OpencodeJson) return "mcp";
    return "mcpServers";
}

std::string tomlString(const std::string& s) {
    if (s.find('\'') == std::string::npos && s.find('\n') == std::string::npos) return "'" + s + "'";
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Removes [mcp_servers.shapeforge] (and its sub-tables) from a TOML document.
std::string tomlWithoutServer(const std::string& text, bool& removed) {
    std::istringstream in(text);
    std::string line, out;
    bool skipping = false;
    removed = false;
    while (std::getline(in, line)) {
        std::string t = line.substr(std::min(line.size(), line.find_first_not_of(" \t")));
        if (!t.empty() && t[0] == '[') {
            skipping = t.rfind("[mcp_servers.shapeforge]", 0) == 0 || t.rfind("[mcp_servers.shapeforge.", 0) == 0;
            if (skipping) removed = true;
        }
        if (!skipping) out += line + "\n";
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    return out;
}

std::string codexSection(const std::string& workdir) {
    Json e = serverEntry(workdir);
    std::string args;
    for (const auto& a : e["args"].asArray()) args += (args.empty() ? "" : ", ") + tomlString(a.asString());
    return "[mcp_servers.shapeforge]\ncommand = " + tomlString(e["command"].asString()) + "\nargs = [" + args + "]\n";
}

Json readJsonConfig(const ClientInfo& c, const std::string& snippet) {
    fs::path path(c.configPath);
    if (c.format == ClientFormat::OpencodeJson && !fs::exists(path) && fs::exists(path.string() + "c"))
        throw std::runtime_error("OpenCode uses " + path.string() + "c (JSON with comments); add this to its \"mcp\" section yourself:\n" + snippet);
    if (!fs::exists(path)) return Json::object();
    std::string text = readText(path);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return Json::object();
    Json doc;
    try {
        doc = Json::parse(text);
    } catch (const std::exception&) {
        throw std::runtime_error(path.string() + " is not plain JSON (comments?); add this entry yourself:\n" + snippet);
    }
    if (!doc.isObject()) throw std::runtime_error(path.string() + " does not contain a JSON object");
    return doc;
}

std::string installOne(const ClientInfo& c, const std::string& workdir) {
    if (c.format == ClientFormat::ClaudeCli) {
        std::string cmd = "claude mcp remove --scope user shapeforge";
#if defined(_WIN32)
        cmd += " >NUL 2>&1";
#else
        cmd += " >/dev/null 2>&1";
#endif
        std::system(cmd.c_str());
        cmd = "claude mcp add --scope user shapeforge -- " + launchCommandLine() + " mcp --workdir " + quoteArg(workdir);
        if (std::system(cmd.c_str()) != 0)
            throw std::runtime_error("could not run the 'claude' CLI. Run this yourself:\n  " + cmd);
        return "Claude Code: registered 'shapeforge'. Start a new session to use it.\n";
    }
    fs::path path(c.configPath);
    if (c.format == ClientFormat::CodexToml) {
        bool removed = false;
        std::string text = fs::exists(path) ? tomlWithoutServer(readText(path), removed) : "";
        if (!text.empty() && text.back() != '\n') text += "\n";
        writeText(path, text + (text.empty() ? "" : "\n") + codexSection(workdir));
    } else {
        Json entry = clientEntry(c.format, workdir);
        Json doc = readJsonConfig(c, Json::object({{"shapeforge", entry}}).dump(2));
        if (c.format == ClientFormat::OpencodeJson && !doc.contains("$schema")) {
            Json withSchema = Json::object({{"$schema", "https://opencode.ai/config.json"}});
            for (auto& [k, v] : doc.asObject()) withSchema[k] = v;
            doc = std::move(withSchema);
        }
        doc[serversKey(c.format)]["shapeforge"] = std::move(entry);
        writeText(path, doc.dump(2) + "\n");
    }
    return c.displayName + ": added 'shapeforge' to " + path.string() + " (restart it to load the tools)\n";
}

std::string uninstallOne(const ClientInfo& c) {
    if (c.format == ClientFormat::ClaudeCli) {
        if (std::system("claude mcp remove --scope user shapeforge") != 0)
            throw std::runtime_error("'claude mcp remove' failed");
        return "Claude Code: removed 'shapeforge'\n";
    }
    fs::path path(c.configPath);
    if (!fs::exists(path)) return c.displayName + ": nothing to remove\n";
    if (c.format == ClientFormat::CodexToml) {
        bool removed = false;
        std::string text = tomlWithoutServer(readText(path), removed);
        if (!removed) return c.displayName + ": nothing to remove\n";
        writeText(path, text);
    } else {
        Json doc = readJsonConfig(c, "");
        Json* servers = doc.find(serversKey(c.format));
        if (!servers || !servers->erase("shapeforge")) return c.displayName + ": nothing to remove\n";
        writeText(path, doc.dump(2) + "\n");
    }
    return c.displayName + ": removed 'shapeforge' from " + path.string() + "\n";
}

// "copilot" means both Copilot front ends; "all" means every client that is installed.
std::vector<ClientInfo> resolveClients(const std::string& id, bool& onlyInstalled) {
    std::vector<ClientInfo> out;
    onlyInstalled = id == "all" || id == "copilot";
    for (const auto& c : knownClients()) {
        if (id == "all" || c.id == id || (id == "copilot" && (c.id == "vscode" || c.id == "copilot-cli")))
            out.push_back(c);
    }
    if (out.empty()) throw std::runtime_error("unknown client '" + id + "' (use " + kClientIds + ")");
    return out;
}

bool clientInstalled(const ClientInfo& c) {
    if (c.format == ClientFormat::ClaudeCli) return true;
    std::error_code ec;
    return fs::is_directory(c.appDir, ec);
}

}  // namespace

std::string clientStatus(const ClientInfo& c) {
    if (c.format == ClientFormat::ClaudeCli) return "run 'claude mcp list' to check";
    if (!clientInstalled(c)) return "not installed";
    std::error_code ec;
    if (!fs::exists(c.configPath, ec)) return "not configured";
    std::string text = readText(c.configPath);
    if (c.format == ClientFormat::CodexToml)
        return text.find("[mcp_servers.shapeforge]") != std::string::npos ? "configured" : "not configured";
    try {
        Json doc = Json::parse(text);
        return doc[serversKey(c.format)].find("shapeforge") ? "configured" : "not configured";
    } catch (const std::exception&) {
        return text.find("\"shapeforge\"") != std::string::npos ? "configured" : "not configured";
    }
}

std::string clientConfigInstructions(const std::string& workdir) {
    std::ostringstream os;
    std::string exe = launchCommandLine();
    os << "Easiest: let shapeforge register itself\n"
       << "  " << exe << " install <client>\n"
       << "  clients: " << kClientIds << "\n\n"
       << "Or add it by hand:\n\n"
       << "Claude Code\n  claude mcp add --scope user shapeforge -- " << exe << " mcp --workdir " << quoteArg(workdir) << "\n\n"
       << "Claude Desktop, Cursor, Windsurf (\"mcpServers\")\n"
       << Json::object({{"mcpServers", Json::object({{"shapeforge", serverEntry(workdir)}})}}).dump(2) << "\n\n"
       << "VS Code / GitHub Copilot (mcp.json, \"servers\")\n"
       << Json::object({{"servers", Json::object({{"shapeforge", clientEntry(ClientFormat::VscodeJson, workdir)}})}}).dump(2)
       << "\n\nGitHub Copilot CLI (~/.copilot/mcp-config.json)\n"
       << Json::object({{"mcpServers", Json::object({{"shapeforge", clientEntry(ClientFormat::CopilotCliJson, workdir)}})}}).dump(2)
       << "\n\nOpenCode (opencode.json, \"mcp\")\n"
       << Json::object({{"mcp", Json::object({{"shapeforge", clientEntry(ClientFormat::OpencodeJson, workdir)}})}}).dump(2)
       << "\n\nOpenAI Codex (~/.codex/config.toml)\n"
       << codexSection(workdir) << "\nConfig files on this machine:\n";
    for (const auto& c : knownClients())
        if (!c.configPath.empty()) os << "  " << c.displayName << ": " << c.configPath << "\n";
    os << "\nModels are written to: " << workdir << "\n";
    return os.str();
}

std::string installIntoClient(const std::string& clientId, const std::string& workdir) {
    std::error_code ec;
    fs::create_directories(workdir, ec);
    bool onlyInstalled = false;
    std::string report;
    for (const auto& c : resolveClients(clientId, onlyInstalled)) {
        if (onlyInstalled && !clientInstalled(c)) {
            report += c.displayName + ": not installed, skipped\n";
            continue;
        }
        if (!onlyInstalled && c.id == "claude-desktop" && !clientInstalled(c))
            throw std::runtime_error("Claude Desktop does not seem to be installed (" + c.appDir + " not found)");
        try {
            report += installOne(c, workdir);
        } catch (const std::exception& e) {
            if (!onlyInstalled) throw;
            report += c.displayName + ": skipped (" + e.what() + ")\n";
        }
    }
    return report;
}

std::string uninstallFromClient(const std::string& clientId) {
    bool onlyInstalled = false;
    std::string report;
    for (const auto& c : resolveClients(clientId, onlyInstalled)) {
        if (onlyInstalled && !clientInstalled(c)) continue;
        try {
            report += uninstallOne(c);
        } catch (const std::exception& e) {
            if (!onlyInstalled) throw;
            report += c.displayName + ": " + e.what() + "\n";
        }
    }
    return report.empty() ? "nothing to remove\n" : report;
}

}  // namespace sf
