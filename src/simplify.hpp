// Mesh post-processing for SDF/CSG output. Surface nets produce a uniform,
// dense triangulation (tens of thousands of triangles even for flat faces);
// quadric-error edge collapse removes the triangles that carry no shape, and
// crease-aware normals keep sharp CSG edges crisp after simplification.
#pragma once

#include "mesh.hpp"

namespace sf {

// Collapses edges while the quadric error (squared distance to the original
// surface's planes) stays below maxError^2. Keeps the mesh manifold and avoids
// flipping triangles. Operates on indexed meshes with shared vertices.
void simplifyMesh(Mesh& mesh, float maxError);

// Recomputes normals: smooth across edges flatter than creaseDeg, split (hard)
// across sharper ones. Duplicates vertices where needed.
void computeCreaseNormals(Mesh& mesh, float creaseDeg);

}  // namespace sf
