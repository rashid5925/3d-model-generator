// Renderer interface plus the backend-independent part of rendering: camera
// setup for named views, grid/axis helpers, overlays (labels, axis gizmo) and
// compositing several views into one image an LLM can look at.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "image.hpp"
#include "json.hpp"
#include "math.hpp"
#include "scene.hpp"

namespace sf {

struct Vertex {
    float pos[3];
    float normal[3];  // all-zero normal = unlit (used for lines)
    float color[3];   // sRGB 0..1
};

// Matches the push-constant block in shaders/mesh.{vert,frag}. 128 bytes.
struct FrameUniforms {
    float viewProj[16];
    float eye[4];      // xyz camera position, w = 1 for orthographic
    float forward[4];  // view direction
    float keyDir[4];   // direction toward the key light
    float fillDir[4];  // direction toward the fill light
};
static_assert(sizeof(FrameUniforms) == 128, "push constant block must be 128 bytes");

struct Frame {
    int width = 512, height = 512;
    FrameUniforms uniforms{};
    Mat4 viewProj;
    Vec3 background{0.23f, 0.24f, 0.27f};
    std::vector<Vertex> triangles;
    std::vector<Vertex> lines;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual std::string name() const = 0;
    // Throws std::runtime_error on failure.
    virtual Image render(const Frame& frame) = 0;
};

std::unique_ptr<Renderer> createCpuRenderer();
#ifdef SF_HAVE_VULKAN
std::unique_ptr<Renderer> createVulkanRenderer();  // throws if Vulkan is unusable
#endif
// preference: "auto" (Vulkan, falling back to CPU), "vulkan" or "cpu".
std::unique_ptr<Renderer> createRenderer(const std::string& preference, std::string& log);

struct RenderOptions {
    std::vector<Json> views;  // names ("front", "iso", ...) or {"azimuth","elevation",...}
    int size = 512;           // per view, in pixels
    std::vector<std::string> highlight;
    std::vector<std::string> focus;  // frame the camera on these (and their children)
    bool isolate = false;            // draw only the focused objects
    bool labels = false;
    bool grid = true;
    bool overlays = true;  // view titles and axis gizmo
};

struct RenderOutput {
    Image image;
    Json info;
};

// Throws SceneError for invalid view specs.
RenderOutput renderScene(Renderer& renderer, const BuildResult& scene, const RenderOptions& options);

const char* viewReference();

}  // namespace sf
