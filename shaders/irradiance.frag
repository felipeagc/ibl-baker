#version 450

layout(location = 0) in vec3 world_pos;

layout(set = 0, binding = 0) uniform samplerCube skybox;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;
const float delta_phi = (2.0f * PI) / 180.0f;
const float delta_theta = (0.5f * PI) / 64.0f;

void main() {
  vec3 N = normalize(world_pos);
  vec3 irradiance = vec3(0.0);  

  vec3 up = vec3(0.0, 1.0, 0.0);
  vec3 right = cross(up, N);
  up = cross(N, right);

  uint nr_samples = 0; 
  for(float phi = 0.0; phi < 2.0 * PI; phi += delta_phi) {
    for(float theta = 0.0; theta < 0.5 * PI; theta += delta_theta) {
			vec3 temp_vec = cos(phi) * right + sin(phi) * up;
			vec3 sample_vector = cos(theta) * N + sin(theta) * temp_vec;
			irradiance += texture(skybox, sample_vector).rgb * cos(theta) * sin(theta);

      nr_samples++;
    }
  }

  irradiance = PI * irradiance / float(nr_samples);

  out_color = vec4(irradiance, 1.0);
}
