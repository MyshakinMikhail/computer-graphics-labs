#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 0) out vec3 vertexColor;
layout(location = 1) out vec3 worldNormal;
layout(set = 0, binding = 0) uniform ObjectData { mat4 mvp; mat4 model; vec4 tint; } object;
void main() {
    gl_Position = object.mvp * vec4(inPosition, 1.0);
    vertexColor = inColor;
    worldNormal = normalize(mat3(transpose(inverse(object.model))) * inNormal);
}
