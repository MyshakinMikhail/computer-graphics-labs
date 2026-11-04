#version 450
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 textureUV;
layout(location = 0) out vec4 outColor;
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D specularMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;
layout(push_constant) uniform Sampling {
    uvec4 flags;          // animated, specular enabled, emissive enabled, reserved
    vec4 animation;       // time, blend strength, speed, emission intensity
    vec4 uvTransform;     // tiling.xy, offset.zw
    vec4 tint;
} effect;
vec3 diffuseAlbedo, specularColor;
vec2 uvA, uvB;
float layerBlend;
vec3 sampleLayers(sampler2D image) {
    vec3 base = texture(image, uvA).rgb;
    if (effect.flags.x == 0u) return base;
    return mix(base, texture(image, uvB).rgb, layerBlend);
}
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
    return radiance * (diffuseAlbedo * diffuse + specularColor * specular);
}
void main() {
    vec2 uv = textureUV * effect.uvTransform.xy + effect.uvTransform.zw;
    uvA = uv;
    uvB = uv;
    layerBlend = 0.0;
    float t = effect.animation.x * effect.animation.z;
    if (effect.flags.x != 0u) {
        // Two time-dependent reads of the SAME image with different coordinates.
        float angle = 0.22 * sin(t * 0.7);
        mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
        uvA = uv + vec2(0.07, -0.05) * t;
        uvB = rotation * (uv * 1.65 - 0.5) + 0.5
            + vec2(-0.06, 0.04) * t + 0.035 * sin(uv.yx * 12.0 + t);
        layerBlend = effect.animation.y * (0.35 + 0.25 * sin(t));
    }
    diffuseAlbedo = sampleLayers(albedoMap) * object.albedo.rgb;
    if (effect.flags.x != 0u) {
        float pulse = effect.animation.y * (0.5 + 0.5 * sin(t * 1.3));
        diffuseAlbedo *= mix(vec3(1.0), effect.tint.rgb, pulse);
    }
    specularColor = effect.flags.y != 0u
        ? sampleLayers(specularMap) * object.specularShininess.rgb : vec3(0.0);
    vec3 emission = effect.flags.z != 0u
        ? sampleLayers(emissiveMap) * effect.animation.w : vec3(0.0);
    vec3 N = safeNormalize(worldNormal);
    vec3 V = safeNormalize(scene.cameraPosition.xyz - worldPosition);
    vec3 color = diffuseAlbedo * scene.ambient.rgb * scene.ambient.w;
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
    // Emission bypasses Blinn-Phong and remains visible with every light disabled.
    color += emission;
    outColor = vec4(color / (vec3(1.0) + color), 1.0);
}
