#version 410 core

layout (location = 0) in vec3 vert_pos;
layout (location = 1) in vec3 vert_offset;
layout (location = 2) in vec3 vert_color;

uniform mat4 projection_matrix;
uniform mat4 view_matrix;
uniform mat4 model_matrix;

out vec3 out_vert_color;

void main()
{
    gl_Position = projection_matrix * view_matrix * (model_matrix * vec4(vert_pos, 1.0) + vec4(vert_offset, 1.0));
    out_vert_color = vert_color;
}