#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;

layout(set = 0, binding = 0) uniform Ubo {
    float angle;   /* seconds since start, the NVN uniform block */
} u;

layout(location = 0) out vec3 vColor;

void main() {
    float c = cos(u.angle);
    float s = sin(u.angle);
    vec2 p = vec2(c * aPos.x - s * aPos.y, s * aPos.x + c * aPos.y) * 0.9;
    gl_Position = vec4(p, 0.0, 1.0);
    vColor = aColor;
}
