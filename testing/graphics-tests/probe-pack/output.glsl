#include <metal_stdlib>
using namespace metal;

struct VertexOut {
	float4 position [[position]];
	float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]]) {
	return float4(textureSrc.sample(samplr, in.uv).gbr, 1.0);
}
