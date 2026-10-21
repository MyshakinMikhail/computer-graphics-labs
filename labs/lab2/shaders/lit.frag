#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;
struct PointLight { vec4 position; vec4 colorIntensity; };
struct SpotLight { vec4 position; vec4 directionInner; vec4 colorIntensity; vec4 outer; };
// Runtime-sized arrays inside two SSBOs; no descriptor indexing feature is needed.
layout(set = 0, binding = 1, std430) readonly buffer PointLights { PointLight points[]; };
layout(set = 0, binding = 2, std430) readonly buffer SpotLights { SpotLight spots[]; };
vec3 safeNormalize(vec3 v) { return v * inversesqrt(max(dot(v,v), 1e-10)); }
vec3 blinnPhong(vec3 N, vec3 V, vec3 L, vec3 radiance) {
    float diffuse = max(dot(N, L), 0.0);
    if (diffuse <= 0.0) return vec3(0.0);
    vec3 H = safeNormalize(L + V);
    float specular = pow(max(dot(N, H), 0.0), object.specularShininess.w);
    return radiance * (object.albedo.rgb * diffuse + object.specularShininess.rgb * specular);
}
void main() {
    vec3 N = safeNormalize(worldNormal);
    vec3 V = safeNormalize(scene.cameraPosition.xyz - worldPosition);
    vec3 color = object.albedo.rgb * scene.ambient.rgb * scene.ambient.w;
    color += blinnPhong(N, V, -scene.directionalDirection.xyz,
                        scene.directionalColor.rgb * scene.directionalColor.w);
    for (uint i = 0; i < scene.lightCounts.x; ++i) {
        PointLight light = points[i];
        vec3 delta = light.position.xyz - worldPosition;
        // Inverse-square falloff, clamped only within 0.1 world units of the source.
        float attenuation = 1.0 / max(dot(delta, delta), 0.01);
        color += blinnPhong(N, V, safeNormalize(delta),
                           light.colorIntensity.rgb * light.colorIntensity.w * attenuation);
    }
    for (uint i = 0; i < scene.lightCounts.y; ++i) {
        SpotLight light = spots[i];
        vec3 delta = light.position.xyz - worldPosition;
        vec3 L = safeNormalize(delta);
        float theta = dot(-L, light.directionInner.xyz);
        float width = light.directionInner.w - light.outer.x;
        // Equal inner/outer angles intentionally produce a hard edge without 0/0.
        float cone = width < 1e-6 ? step(light.outer.x, theta)
                                 : clamp((theta - light.outer.x) / width, 0.0, 1.0);
        float attenuation = cone / max(dot(delta, delta), 0.01);
        color += blinnPhong(N, V, L, light.colorIntensity.rgb * light.colorIntensity.w * attenuation);
    }
    outColor = vec4(color, 1.0);
}
