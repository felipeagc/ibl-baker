#version 450

layout(location = 0) in vec3 world_pos;

layout(set = 0, binding = 0) uniform sampler2D equirectangular_map;

layout(location = 0) out vec4 out_color;

const vec2 inv_atan = vec2(0.1591, 0.3183);
const float PI = 3.14159265359;

vec2 sample_spherical_map(vec3 v) {
  vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
  uv *= inv_atan;
  uv += 0.5;
  return uv;
}

void main() {
  vec3 N = normalize(world_pos);
  vec3 irradiance = vec3(0.0);  

  vec3 up = vec3(0.0, 1.0, 0.0);
  vec3 right = cross(up, N);
  up = cross(N, right);

  float sample_delta = 0.025;
  float nr_samples = 0.0; 
  for(float phi = 0.0; phi < 2.0 * PI; phi += sample_delta) {
    for(float theta = 0.0; theta < 0.5 * PI; theta += sample_delta) {
      // spherical to cartesian (in tangent space)
      vec3 tangent_sample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
      // tangent space to world
      vec3 sample_vec = tangent_sample.x * right + tangent_sample.y * up + tangent_sample.z * N; 

      vec2 uv = sample_spherical_map(normalize(sample_vec));
      uv.y = 1.0 - uv.y;

      irradiance += texture(equirectangular_map, uv).rgb * cos(theta) * sin(theta);
      nr_samples++;
    }
  }

  irradiance = PI * irradiance * (1.0 / float(nr_samples));

  out_color = vec4(irradiance, 1.0);
}
