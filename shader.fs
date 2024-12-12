#version 410 core

in vec3 out_vert_color;

out vec4 frag_color;

void main()
{
	frag_color = vec4(out_vert_color, 1.f);
}