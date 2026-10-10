// Placement helpers that compute exact positions from geometry.
//   place_on  - rest an object on top of (or against a side of) another object
//               or the ground, touching its actual surface, not just its box.
//   align     - line objects up on an axis (min / center / max).
//   mirror    - mirrored copy of an object (and its children) across a plane.
//   array     - linear or radial copies.
// All operations edit object specs (position/rotation/scale), so results stay
// editable and are saved with the scene. Objects with children move as a unit.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "scene.hpp"

namespace sf {

struct PlacementResult {
    std::vector<std::string> changed;  // objects created or moved
    std::vector<std::string> notes;
};

// Each throws SceneError on invalid arguments. `args` is the tool's JSON input.
PlacementResult placeOn(Scene& scene, const Json& args);
PlacementResult alignObjects(Scene& scene, const Json& args);
PlacementResult mirrorObject(Scene& scene, const Json& args);
PlacementResult arrayObject(Scene& scene, const Json& args);

// Distance `moving` can travel along axis `axis` (0..2) in direction `sign`
// (+1/-1) before touching `fixed`; returns a negative value when it never does.
float sweepDistance(const Mesh& moving, const Mesh& fixed, int axis, int sign);

}  // namespace sf
