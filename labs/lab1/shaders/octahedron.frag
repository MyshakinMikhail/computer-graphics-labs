#version 450
layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform ObjectData { mat4 mvp; mat4 model; vec4 tint; } object;
void main() {
    float light = 0.45 + 0.55 * max(dot(normalize(worldNormal),
                                         normalize(vec3(0.5, 0.8, 1.0))), 0.0);
    outColor = vec4(vertexColor * object.tint.rgb * light, 1.0);
}
