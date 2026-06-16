#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
layout(location=3) in float a_biome;
layout(location=4) in float a_height_norm;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;

out vec3 v_normal;
out vec3 v_world_pos;
out vec2 v_uv;
flat out float v_biome; // no interpolation: avoids biome rainbow-stripe edges
out float v_height_norm;

// Sum of directional Gerstner waves. Returns vertical offset and writes the
// analytic surface tangent/bitangent so the fragment shader gets a correct,
// smoothly-varying normal (the old code displaced height but kept the flat
// plane normal -> hard faceted "folds"). This is the standard public ocean
// wave model (Tessendorf / Finch, GPU Gems 1).
float gerstner(vec2 xz, out vec3 dx, out vec3 dz) {
    // wave params: dir, amplitude, wavelength, speed, steepness
    const int N = 4;
    vec2  dir[N];   float amp[N]; float len[N]; float spd[N]; float stp[N];
    dir[0]=normalize(vec2( 1.0, 0.3)); amp[0]=0.45; len[0]=22.0; spd[0]=1.1; stp[0]=0.8;
    dir[1]=normalize(vec2(-0.6, 1.0)); amp[1]=0.30; len[1]=13.0; spd[1]=1.4; stp[1]=0.7;
    dir[2]=normalize(vec2( 0.8,-0.7)); amp[2]=0.18; len[2]= 7.0; spd[2]=1.8; stp[2]=0.6;
    dir[3]=normalize(vec2(-0.2,-1.0)); amp[3]=0.10; len[3]= 4.0; spd[3]=2.4; stp[3]=0.5;
    float y = 0.0;
    dx = vec3(1.0, 0.0, 0.0);
    dz = vec3(0.0, 0.0, 1.0);
    for (int i = 0; i < N; i++) {
        float k = 6.28318 / len[i];
        float f = k * dot(dir[i], xz) + u_time * spd[i];
        float a = amp[i];
        float c = cos(f), s = sin(f);
        y += a * s;
        // partial derivatives of the displaced surface for the normal
        float wa = k * a;
        dx.y += dir[i].x * wa * c;
        dz.y += dir[i].y * wa * c;
    }
    return y;
}

void main() {
    vec3 pos = a_pos;
    vec3 nrm = a_normal;
    if (int(a_biome + 0.5) == 5) {
        vec3 dx, dz;
        pos.y += gerstner(pos.xz, dx, dz);
        // Surface normal from the two analytic tangents.
        nrm = normalize(cross(dz, dx));
    }
    v_world_pos = pos;
    v_normal = nrm;
    v_uv = a_uv;
    v_biome = a_biome;
    v_height_norm = a_height_norm;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
}
