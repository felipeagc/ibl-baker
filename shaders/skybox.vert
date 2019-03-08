#version 450

vec3 pos[36] = vec3[](
    vec3(-1.0, 1.0, -1.0),
    vec3(-1.0, -1.0, -1.0),
    vec3(1.0, -1.0, -1.0),
    vec3(1.0, -1.0, -1.0),
    vec3(1.0, 1.0, -1.0),
    vec3(-1.0, 1.0, -1.0),

    vec3(-1.0, -1.0, 1.0),
    vec3(-1.0, -1.0, -1.0),
    vec3(-1.0, 1.0, -1.0),
    vec3(-1.0, 1.0, -1.0),
    vec3(-1.0, 1.0, 1.0),
    vec3(-1.0, -1.0, 1.0),

    vec3(1.0, -1.0, -1.0),
    vec3(1.0, -1.0, 1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(1.0, 1.0, -1.0),
    vec3(1.0, -1.0, -1.0),

    vec3(-1.0, -1.0, 1.0),
    vec3(-1.0, 1.0, 1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(1.0, -1.0, 1.0),
    vec3(-1.0, -1.0, 1.0),

    vec3(-1.0, 1.0, -1.0),
    vec3(1.0, 1.0, -1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(1.0, 1.0, 1.0),
    vec3(-1.0, 1.0, 1.0),
    vec3(-1.0, 1.0, -1.0),

    vec3(-1.0, -1.0, -1.0),
    vec3(-1.0, -1.0, 1.0),
    vec3(1.0, -1.0, -1.0),
    vec3(1.0, -1.0, -1.0),
    vec3(-1.0, -1.0, 1.0),
    vec3(1.0, -1.0, 1.0)
);

out gl_PerVertex {
  vec4 gl_Position;
};

layout (push_constant) uniform PushConstant {
  mat4 mvp;
  float roughness;
} pc;

layout (location = 0) out vec3 world_pos;

void main() {
  world_pos = pos[gl_VertexIndex];
  gl_Position = pc.mvp * vec4(world_pos, 1.0);
}
