#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform Frame {
    mat4 viewProj;
    vec4 eye;
    vec4 forward;
    vec4 keyDir;
    vec4 fillDir;
} pc;

layout(location = 0) out vec3 vPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;

void main() {
    vPos = inPos;
    vNormal = inNormal;
    vColor = inColor;
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
}
