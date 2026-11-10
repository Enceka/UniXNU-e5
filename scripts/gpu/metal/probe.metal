#include <metal_stdlib>
using namespace metal;
kernel void e5_square(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i < 1024) out[i] = i * i + 17;
}
kernel void e5_import_roundtrip(device uint *values [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i < 1024) values[i] = values[i] * 7 + 17;
}
kernel void e5_readonly(device const uint *input [[buffer(0)]],
                        device uint *output [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    if (i < 1024) output[i] = input[i] * 7 + 17;
}
kernel void e5_read_buffer_image(texture2d<float, access::read> input [[texture(0)]],
                                  device uint *output [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i < 1024) {
        uint4 c = uint4(input.read(uint2(i % 64, i / 64)) * 255.0f + 0.5f);
        output[i] = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    }
}
struct Vertex { float4 position [[position]]; };
vertex Vertex e5_triangle(uint i [[vertex_id]]) {
    const float2 p[3] = {float2(-0.5, 0.75), float2(0.5, 0.75), float2(0.0, -0.75)};
    return {float4(p[i], 0.0, 1.0)};
}
fragment float4 e5_yellow(Vertex in [[stage_in]]) {
    return float4(1.0, 1.0, 0.0, 1.0);
}
struct FetchVertex { float4 position [[position]]; uint step [[flat]]; };
vertex FetchVertex e5_fetch_vertex(uint i [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return {float4(positions[i % 3], 0, 1), i / 3};
}
fragment uint4 e5_fetch_uint(FetchVertex in [[stage_in]], uint4 previous [[color(0)]]) {
    return uint4((previous.r * 5 + in.step + 1) & 255, in.step, 0, 255);
}
fragment float4 e5_fetch_unorm(FetchVertex in [[stage_in]], float4 previous [[color(0)]]) {
    return float4(previous.r * 0.5 + float(in.step + 1) / 64.0, 0, 0, 1);
}
vertex FetchVertex e5_vary_constant(uint i [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return {float4(positions[i % 3], 0, 1), 0x12340025u};
}
fragment uint4 e5_vary_uint(FetchVertex in [[stage_in]]) {
    return uint4(in.step & 255, in.step & 255, 0, 255);
}
