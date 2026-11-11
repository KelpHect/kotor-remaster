#version 450

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec2 vTexCoord1;
layout(location = 4) in vec4 vTint;
layout(location = 5) in vec3 vLit; // scene.vert: lit per vertex, as retail's fixed-function GL

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    vec4 modelRows[3];
    mat4 viewProjection;
    vec4 ambient;           // rgb, w = fog far
    vec4 lightPositions[3]; // xyz, w = quadratic attenuation (< 0: a direction)
    uint lightColours[4];   // RGB8 of colour / 4; the last: bump cells x, y, cell x, y (5 bits each; 0 = none), specular (8 bits)
    vec4 fogColor;          // rgb, w = fog on
    vec4 eye;               // xyz, w = fog near
    vec4 uv;                // scale u v, offset u v
    vec4 params;            // alpha reference, lightmap, environment map (2 + a: map only, at alpha a),
                            // unlit (2: also output alpha 1, 3: force shell)
} pc;

layout(set = 0, binding = 0) uniform sampler2D sceneTexture;
layout(set = 1, binding = 0) uniform sampler2D lightmapTexture;
layout(set = 2, binding = 0) uniform sampler2D envTexture;
layout(set = 3, binding = 0) uniform sampler2D bumpTexture;

// ProgFX 1800 distortion (params.w 4), same maths as the GLSL in GlScene.cpp. The ring texture
// (distortiontex.txi: ringtexdistort 32 x 32, built by 00460a90): texel (x, y) holds
// 255 x (0.5 + 0.03 x cos32(25 phase + 60 r)), r the distance of ((x + 1) / 32 - 0.5, (y + 1) / 32 - 0.5)
// from 0, cos32 the 32-step table 004aadf0 / 00743fe8; red and green alike, blue 255. Its 32 bytes:
const float kRingBytes[32] = float[32](135.0, 135.0, 134.0, 133.0, 132.0, 131.0, 130.0, 128.0, 127.0, 126.0,
    124.0, 123.0, 122.0, 121.0, 120.0, 119.0, 119.0, 119.0, 120.0, 121.0, 122.0, 123.0, 124.0, 126.0, 127.0,
    128.0, 130.0, 131.0, 132.0, 133.0, 134.0, 135.0);
float ringTexel(ivec2 p, float phase) {
    vec2 f = vec2(p + 1) / 32.0 - 0.5;
    float a = 25.0 * phase + 60.0 * sqrt(f.x * f.x + f.y * f.y);
    return kRingBytes[int(roundEven(a * 5.09295797)) & 31] / 255.0;
}
// GL_LINEAR, clamped (RenderForceDistortion sets GL_CLAMP on the unit; drivers clamp to the edge).
float ringSample(vec2 st, float phase) {
    vec2 p = st * 32.0 - 0.5;
    vec2 f = p - floor(p);
    ivec2 a = clamp(ivec2(floor(p)), ivec2(0), ivec2(31));
    ivec2 b = clamp(ivec2(floor(p)) + 1, ivec2(0), ivec2(31));
    float near = mix(ringTexel(a, phase), ringTexel(ivec2(b.x, a.y), phase), f.x);
    float far = mix(ringTexel(ivec2(a.x, b.y), phase), ringTexel(b, phase), f.x);
    return mix(near, far, f.y);
}

// Same maths as the GLSL in GlScene.cpp.
void main() {
    vec4 texel = texture(sceneTexture, vTexCoord * pc.uv.xy + pc.uv.zw);
    if (pc.params.x >= 0.0 && texel.a <= pc.params.x) {
        discard;
    }
    if (pc.params.w > 3.5) {
        // Vertex program 0078de48 and the texture shader / ATI shader of 004331d0 / 00432d70: the frame
        // (set 2 here) is read at the pixel moved by 0.1 x the expanded ring texel, x right and y up
        // (nz = 1), the ring stretched once over the screen from its top left. Vulkan's frame copy and
        // gl_FragCoord run top-down. lightPositions[0] = 1/w, 1/h, phase.
        vec2 uv = gl_FragCoord.xy * pc.lightPositions[0].xy;
        float n = ringSample(uv, pc.lightPositions[0].z) * 2.0 - 1.0;
        fragColor = vec4(texture(envTexture, vec2(uv.x + 0.1 * n, uv.y - 0.1 * n)).rgb, 1.0);
        return;
    }
    vec3 normal = normalize(vNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    if (pc.lightColours[3] != 0u) {
        // Program 83025c dots the map's normal with the mesh's frame; here the frame comes from the
        // screen derivatives (engine choice). Same maths as the GLSL in GlScene.cpp.
        vec2 cells = vec2(float(pc.lightColours[3] & 31u), float((pc.lightColours[3] >> 5u) & 31u));
        vec2 cell = vec2(float((pc.lightColours[3] >> 10u) & 31u), float((pc.lightColours[3] >> 15u) & 31u));
        vec3 mapNormal = textureGrad(bumpTexture, (fract(vTexCoord) + cell) / cells, dFdx(vTexCoord) / cells, dFdy(vTexCoord) / cells).rgb * 2.0 - 1.0;
        vec3 dp1 = dFdx(vWorld);
        vec3 dp2 = dFdy(vWorld);
        vec2 d1 = dFdx(vTexCoord);
        vec2 d2 = dFdy(vTexCoord);
        float det = d1.x * d2.y - d2.x * d1.y;
        if (det != 0.0) {
            float s = det < 0.0 ? -1.0 : 1.0;
            vec3 t = (dp1 * d2.y - dp2 * d1.y) * s;
            vec3 b = (dp2 * d1.x - dp1 * d2.x) * s;
            t -= normal * dot(normal, t);
            b -= normal * dot(normal, b);
            if (dot(t, t) > 0.0 && dot(b, b) > 0.0) {
                normal = normalize(normalize(t) * mapNormal.x + normalize(b) * mapNormal.y + normal * mapNormal.z);
            }
        }
    }
    vec3 color;
    vec3 litTerm = vec3(1.0);
    float alpha = texel.a * vTint.a;
    if (pc.params.w > 2.5) {
        // Force shell: the program's colour is env[12].w = 1, so the pass is the texel, unlit, at its own alpha.
        color = texel.rgb;
        alpha = texel.a;
    } else if (pc.params.w > 0.5) {
        litTerm = vTint.rgb;
        color = texel.rgb * vTint.rgb;
    } else if (pc.params.y < 0.5 && pc.lightColours[3] == 0u) {
        litTerm = vLit;
        color = texel.rgb * vLit;
    } else {
        vec3 light;
        vec3 specular = vec3(0.0);
        if (pc.params.y > 0.5) {
            light = texture(lightmapTexture, vTexCoord1).rgb;
        } else {
            light = pc.ambient.rgb;
            float bumpSpecular = float((pc.lightColours[3] >> 20u) & 255u) / 255.0;
            for (int i = 0; i < 3; ++i) {
                vec3 toLight = pc.lightPositions[i].xyz;
                float attenuation = 1.0;
                if (pc.lightPositions[i].w >= 0.0) {
                    toLight -= vWorld;
                    attenuation = 1.0 / (1.0 + pc.lightPositions[i].w * dot(toLight, toLight));
                    toLight = normalize(toLight);
                }
                light += unpackUnorm4x8(pc.lightColours[i]).rgb * 4.0 * attenuation * max(dot(normal, toLight), 0.0);
                if (bumpSpecular > 0.0) {
                    // Program 830250: attenuation * con0 * (N.H)^8 * (1 - texel alpha).
                    float nh = max(dot(normal, normalize(toLight + normalize(pc.eye.xyz - vWorld))), 0.0);
                    specular += unpackUnorm4x8(pc.lightColours[i]).rgb * 4.0 * attenuation * bumpSpecular * pow(nh, 8.0);
                }
            }
        }
        // The exe sets no colour scale (no RGB_SCALE push), so a lit colour tops out at the texel.
        litTerm = min(light + vTint.rgb, vec3(1.0));
        color = texel.rgb * litTerm + specular * (1.0 - texel.a);
    }
    if (pc.params.z > 0.5) {
        vec3 r = reflect(normalize(vWorld - pc.eye.xyz), normal);
        vec2 ll = vec2(atan(r.y, r.x) / 6.2831853 + 0.5, acos(clamp(r.z, -1.0, 1.0)) / 3.14159265);
        vec3 env = texture(envTexture, ll).rgb;
        if (pc.params.z > 1.5) {
            color = env;
            alpha = vTint.a * (pc.params.z - 2.0);
        } else {
            // Retail ATI program 830258 with a lightmap adds the environment. The bump route 00470860 adds with or
            // without a lightmap. Without a lightmap, retail's route without fragment shaders (0046ff70, the one
            // the retail captures take) draws the cube map alone under the vertex lighting (texture env MODULATE),
            // then the texture over it with SRC_ALPHA / ONE_MINUS_SRC_ALPHA (0073f270): the reflection is lit.
            color = pc.params.y > 0.5 || pc.lightColours[3] != 0u ? color + env * (1.0 - texel.a) : mix(env * (pc.params.z > 1.1 ? vec3(1.0) : litTerm), color, texel.a);
            alpha = vTint.a;
        }
    }
    if (pc.params.w > 1.5 && pc.params.w < 2.5) {
        alpha = 1.0;
    }
    if (pc.fogColor.w > 0.5) {
        float fog = clamp((length(vWorld - pc.eye.xyz) - pc.eye.w) / max(pc.ambient.w - pc.eye.w, 1.0), 0.0, 1.0);
        color = mix(color, pc.fogColor.rgb, fog);
    }
    fragColor = vec4(color, alpha);
}
