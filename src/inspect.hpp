// Numeric scene analysis: bounds, sizes, volumes, contacts between parts and
// common placement mistakes (floating parts, parts below the ground).
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "scene.hpp"

namespace sf {

// names: objects to report (empty = all). With 2+ names, every pair among them
// gets a detailed relation (per-axis gaps).
Json inspectScene(const BuildResult& scene, const std::vector<std::string>& names);

// Compact one-line-per-object summary used after edits.
Json summarizeObject(const BuiltObject& o);

double roundTo(double v, int decimals = 4);
Json vecJson(Vec3 v);

}  // namespace sf
