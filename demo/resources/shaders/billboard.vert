#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

layout(push_constant, std430) uniform constants
{
    mat4 projection;
    mat4 model_view;
};

void main() {
    gl_Position = projection * model_view * vec4(inPosition, 1.0); 
    fragTexCoord = inTexCoord;
}
