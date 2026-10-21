#version 450
layout(push_constant) uniform Marker { mat4 mvp; vec4 color; } marker;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(marker.color.rgb, 1.0); }
