// shapeforge command-line entry point.
//   shapeforge mcp       - run as an MCP server on stdio (what LLM clients launch)
//   shapeforge install   - register this executable with Claude Code / Claude Desktop / Cursor / Windsurf
//   shapeforge doctor    - check that rendering works on this machine
//   shapeforge render | export | inspect - work with scene files directly
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "inspect.hpp"
#include "mcp_server.hpp"
#include "obj_export.hpp"
#include "platform.hpp"
#include "render/renderer.hpp"
#include "scene.hpp"

using namespace sf;

namespace {

const char* kUsage = R"(shapeforge )" SHAPEFORGE_VERSION R"( - 3D modeling tools for AI assistants (MCP server)

Get started:
  shapeforge install claude-code      Register with an AI app: claude-code, claude-desktop, cursor,
                                      windsurf, vscode, copilot-cli, copilot, opencode, codex, all
  shapeforge config                   Show how to register manually with any MCP client
  shapeforge doctor                   Check that everything works on this machine

Commands:
  mcp [--renderer auto|vulkan|cpu] [--workdir DIR]
      Run the MCP server on stdin/stdout (MCP clients start this for you).
  install <client> [--workdir DIR]    uninstall <client>
  render <scene.json> [-o out.png] [--views front,right,top,iso,AZ:EL] [--size 512]
         [--labels] [--no-grid] [--no-overlays] [--clean] [--highlight a,b] [--focus a,b] [--isolate]
         [--renderer auto|vulkan|cpu]
  export <scene.json> [-o out.obj] [--scale 1]
  inspect <scene.json> [--names a,b]
  shapes        Print the shape reference
  version
)";

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty()) out.push_back(item);
    return out;
}

Scene loadSceneFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    Scene scene;
    std::vector<std::string> warnings;
    scene.loadJson(Json::parse(ss.str()), warnings);
    for (const auto& w : warnings) std::cerr << "warning: " << w << "\n";
    return scene;
}

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def) const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

Args parseArgs(int argc, char** argv, int start) {
    static const std::vector<std::string> flags = {"--labels", "--no-grid", "--isolate", "--clean", "--no-overlays"};
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("-", 0) == 0 && s.size() > 1) {
            if (std::find(flags.begin(), flags.end(), s) != flags.end()) a.opts[s] = "1";
            else if (i + 1 < argc) a.opts[s] = argv[++i];
            else throw std::runtime_error("missing value for " + s);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::string defaultOutput(const std::string& scenePath, const std::string& ext) {
    std::filesystem::path p(scenePath);
    p.replace_extension(ext);
    return p.string();
}

int cmdRender(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("render needs a scene file");
    Scene scene = loadSceneFile(a.positional[0]);
    RenderOptions opts;
    for (const auto& v : splitComma(a.get("--views", ""))) {
        size_t colon = v.find(':');  // "azimuth:elevation" for a custom perspective view
        if (colon == std::string::npos) opts.views.push_back(Json(v));
        else opts.views.push_back(Json::object({{"azimuth", std::stod(v.substr(0, colon))}, {"elevation", std::stod(v.substr(colon + 1))}}));
    }
    opts.size = std::stoi(a.get("--size", "512"));
    opts.labels = a.has("--labels");
    opts.grid = !a.has("--no-grid");
    opts.highlight = splitComma(a.get("--highlight", ""));
    opts.focus = splitComma(a.get("--focus", ""));
    opts.isolate = a.has("--isolate");
    if (a.has("--no-overlays")) opts.overlays = false;
    if (a.has("--clean")) opts.overlays = opts.grid = false;
    std::string log;
    auto renderer = createRenderer(a.get("--renderer", "auto"), log);
    std::cerr << log << "\n";
    BuildResult built = scene.build();
    for (const auto& w : built.warnings) std::cerr << "warning: " << w << "\n";
    RenderOutput out = renderScene(*renderer, built, opts);
    std::string path = a.get("-o", defaultOutput(a.positional[0], ".png"));
    if (!writeFile(path, encodePng(out.image))) throw std::runtime_error("cannot write " + path);
    std::cout << "wrote " << path << " (" << out.image.width << "x" << out.image.height << ")\n";
    return 0;
}

int cmdExport(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("export needs a scene file");
    Scene scene = loadSceneFile(a.positional[0]);
    ObjExportOptions opts;
    opts.scale = std::stof(a.get("--scale", "1"));
    ObjExportResult r = exportObj(scene.build(), a.get("-o", defaultOutput(a.positional[0], ".obj")), opts);
    std::cout << "wrote " << r.objPath << " + " << r.mtlPath << " (" << r.objects << " objects, " << r.vertices
              << " vertices, " << r.triangles << " triangles)\n";
    return 0;
}

int cmdInspect(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("inspect needs a scene file");
    Scene scene = loadSceneFile(a.positional[0]);
    std::cout << inspectScene(scene.build(), splitComma(a.get("--names", ""))).dump(2) << "\n";
    return 0;
}

int cmdDoctor() {
    std::cout << "shapeforge " << SHAPEFORGE_VERSION << "\n";
    std::cout << "executable:   " << executablePath() << "\n";
    std::cout << "output dir:   " << defaultWorkdir() << " (default when a client does not pass --workdir)\n\n";

    Scene scene;
    std::vector<std::string> w;
    scene.put(Json::parse(R"({"name": "probe", "shape": "csg", "op": "subtract", "position": [0, 0.5, 0],
        "children": [{"shape": "box", "size": [1, 1, 1]}, {"shape": "sphere", "radius": 0.6}]})"), w);
    BuildResult built = scene.build();
    std::cout << "geometry:     ok (CSG probe: " << built.objects[0].worldMesh.triangleCount() << " triangles)\n";

    bool ok = true;
    for (const char* pref : {"vulkan", "cpu"}) {
        try {
            std::string log;
            auto r = createRenderer(pref, log);
            RenderOptions opts;
            opts.size = 256;
            auto t0 = std::chrono::steady_clock::now();
            RenderOutput out = renderScene(*r, built, opts);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::cout << (std::string(pref) == "vulkan" ? "GPU renderer: " : "CPU renderer: ") << "ok - " << r->name()
                      << " (" << out.image.width << "x" << out.image.height << " in " << ms << " ms)\n";
        } catch (const std::exception& e) {
            std::cout << (std::string(pref) == "vulkan" ? "GPU renderer: " : "CPU renderer: ") << "unavailable - "
                      << e.what() << "\n";
            if (std::string(pref) == "cpu") ok = false;
        }
    }

    std::cout << "\nMCP clients:\n";
    for (const auto& c : knownClients())
        std::cout << "  " << c.displayName << ": " << clientStatus(c)
                  << (c.configPath.empty() ? "" : " (" + c.configPath + ")") << "\n";
    std::cout << (ok ? "\nAll good.\n" : "\nProblems found.\n");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "help")) {
        std::cout << kUsage;
        return argc < 2 ? 1 : 0;
    }
    std::string cmd = argv[1];
    try {
        Args a = parseArgs(argc, argv, 2);
        if (cmd == "mcp") {
            configureStdio();
            std::ios::sync_with_stdio(false);
            ServerConfig cfg;
            cfg.renderer = a.get("--renderer", "auto");
            cfg.workdir = a.get("--workdir", "");
            McpServer server(cfg);
            std::cerr << "[shapeforge] MCP server " << SHAPEFORGE_VERSION << " ready on stdio" << std::endl;
            return server.run(std::cin, std::cout);
        }
        if (cmd == "install" || cmd == "uninstall") {
            if (a.positional.empty())
                throw std::runtime_error(cmd + " needs a client: claude-code, claude-desktop, cursor, windsurf, vscode, copilot-cli, copilot, opencode, codex or all");
            if (cmd == "install") {
                std::string workdir = a.get("--workdir", recommendedWorkdir());
                std::cout << installIntoClient(a.positional[0], std::filesystem::absolute(workdir).string());
                std::cout << "Models will be saved in " << std::filesystem::absolute(workdir).string() << "\n";
            } else {
                std::cout << uninstallFromClient(a.positional[0]);
            }
            return 0;
        }
        if (cmd == "config") {
            std::cout << clientConfigInstructions(a.get("--workdir", recommendedWorkdir()));
            return 0;
        }
        if (cmd == "doctor") return cmdDoctor();
        if (cmd == "render") return cmdRender(a);
        if (cmd == "export") return cmdExport(a);
        if (cmd == "inspect") return cmdInspect(a);
        if (cmd == "shapes") {
            std::cout << shapeReference() << "\n\n" << viewReference() << "\n";
            return 0;
        }
        if (cmd == "version" || cmd == "--version") {
            std::cout << "shapeforge " << SHAPEFORGE_VERSION << "\n";
            return 0;
        }
        std::cerr << "unknown command '" << cmd << "'\n\n" << kUsage;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
