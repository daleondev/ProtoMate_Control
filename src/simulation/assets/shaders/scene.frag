#version 450 core

in vec3 worldNormal;
uniform vec3 color;
uniform int unlit;
out vec4 fragment;

void main()
{
    float light = unlit != 0 ? 1.0 :
        0.28 + 0.72 * max(dot(normalize(worldNormal), normalize(vec3(0.3, -0.5, 1.0))), 0.0);
    fragment = vec4(color * light, 1.0);
}
