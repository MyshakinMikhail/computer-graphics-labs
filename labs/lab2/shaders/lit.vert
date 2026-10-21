#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
void main() {
    vec4 position = object.model * vec4(inPosition, 1.0);
    worldPosition = position.xyz;
    worldNormal = mat3(object.normalMatrix) * inNormal;
    gl_Position = scene.viewProjection * position;
}
