#version 450 core

in vec3 worldNormal;
uniform vec4 color;
out vec4 fragment;

void main()
{
    vec3 n = normalize(worldNormal);
    float light = 0.25
        + 0.6 * max(dot(n, normalize(vec3(0.3, -0.5, 1.0))), 0.0)
        + 0.15 * max(dot(n, normalize(vec3(-0.8, 0.3, 0.5))), 0.0);
    fragment = vec4(color.rgb * light, color.a);
}
