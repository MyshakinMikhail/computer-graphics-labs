#version 450
layout(location = 0) in vec3 inPosition;
layout(push_constant) uniform Marker { mat4 mvp; vec4 color; } marker;
void main() { gl_Position = marker.mvp * vec4(inPosition, 1.0); }
