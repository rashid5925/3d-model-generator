// Model Context Protocol server (JSON-RPC 2.0 over stdio, newline-delimited).
// Exposes the scene as a set of tools an LLM can call: build parts, look at
// them (rendered images), measure them, fix them, and export OBJ.
#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "render/renderer.hpp"
#include "placement.hpp"
#include "scene.hpp"

namespace sf {

struct ServerConfig {
    std::string renderer = "auto";  // auto | vulkan | cpu
    std::string workdir;            // base for relative paths; defaults to defaultWorkdir()
};

class McpServer {
public:
    explicit McpServer(ServerConfig cfg);
    ~McpServer();

    // Serves until EOF on `in`. Returns a process exit code.
    int run(std::istream& in, std::ostream& out);

    // Handles one JSON-RPC message. Returns null for notifications.
    Json handle(const Json& message);

    // Executes a tool and returns an MCP CallToolResult ({content, isError}).
    Json callTool(const std::string& name, const Json& args);

    static Json toolDefinitions();

private:
    Renderer& renderer();
    std::string resolvePath(const std::string& path, const std::string& ext) const;
    void snapshot();

    Json toolAddObjects(const Json& args);
    Json toolUpdateObjects(const Json& args);
    Json toolDeleteObjects(const Json& args);
    Json toolGetScene(const Json& args);
    Json toolClearScene(const Json& args);
    Json toolRender(const Json& args);
    Json toolPlacement(const Json& args, PlacementResult (*op)(Scene&, const Json&));
    Json toolInspect(const Json& args);
    Json toolExportObj(const Json& args);
    Json toolSaveScene(const Json& args);
    Json toolLoadScene(const Json& args);
    Json toolUndo(const Json& args);

    ServerConfig cfg_;
    Scene scene_;
    std::vector<std::vector<Json>> undo_;
    std::unique_ptr<Renderer> renderer_;
};

}  // namespace sf
