#version 450
// GPU twin of src/render/shading.hpp. Keep the two in sync.

layout(push_constant) uniform Frame {
    mat4 viewProj;
    vec4 eye;
    vec4 forward;
    vec4 keyDir;
    vec4 fillDir;
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;

layout(location = 0) out vec4 outColor;

float toLinear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float toSrgb(float c) {
    c = clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

void main() {
    if (dot(vNormal, vNormal) < 1e-4) {  // unlit (grid lines)
        outColor = vec4(vColor, 1.0);
        return;
    }
    vec3 n = normalize(vNormal);
    vec3 v = pc.eye.w > 0.5 ? -pc.forward.xyz : normalize(pc.eye.xyz - vPos);
    if (dot(n, v) < 0.0) n = -n;  // two-sided lighting
    vec3 base = vec3(toLinear(vColor.r), toLinear(vColor.g), toLinear(vColor.b));
    float diffuse = max(dot(n, pc.keyDir.xyz), 0.0) * 0.78 + max(dot(n, pc.fillDir.xyz), 0.0) * 0.22;
    float hemi = 0.5 + 0.5 * n.y;
    float ambient = 0.10 + 0.10 * hemi;
    vec3 h = normalize(pc.keyDir.xyz + v);
    float spec = 0.12 * pow(max(dot(n, h), 0.0), 40.0);
    float facing = 0.78 + 0.22 * max(dot(n, v), 0.0);
    vec3 lin = base * ((ambient + diffuse) * facing) + vec3(spec);
    outColor = vec4(toSrgb(lin.r), toSrgb(lin.g), toSrgb(lin.b), 1.0);
}
