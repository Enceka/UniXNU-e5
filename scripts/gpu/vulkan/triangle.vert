#version 450
const vec2 positions[3]=vec2[3](vec2(-0.5,-0.75),vec2(0.5,-0.75),vec2(0.0,0.75));
void main() { gl_Position=vec4(positions[gl_VertexIndex],0.0,1.0); }
