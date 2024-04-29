/*
 * Bandlimited pixel footprint shader.
 * Author: Themaister
 * License: MIT
 * Adapted from: https://github.com/Themaister/Granite/blob/master/assets/shaders/inc/bandlimited_pixel_filter.h
 * ported to blastem shader format by hunterk
 */

// sensible values between 0.0 and 5.0
#define SMOOTHNESS 0.5
 
uniform sampler2D textures[2];
uniform highp vec2 texsize;

varying highp vec2 texcoord;

// The cosine filter convolved with rect has a support of 0.5 + d pixels.
// We can sample 4x4 regions, so we can deal with 2.0 pixel range in our filter,
// and the maximum extent value we can have is 1.5.
const highp float maximum_support_extent = 1.5;

struct BandlimitedPixelInfo
{
	highp vec2 uv0;
	highp vec2 uv1;
	highp vec2 uv2;
	highp vec2 uv3;
	mediump vec4 weights;
	mediump float l;
};

// Our Taylor approximation is not exact, normalize so the peak is 1.
const highp float taylor_pi_half = 1.00452485553;
const highp float taylor_normalization = 1.0 / taylor_pi_half;
const highp float PI = 3.14159265359;
const highp float PI_half = 0.5 * PI;

#define gen_taylor(T) \
mediump T taylor_sin(mediump T p) \
{ \
	mediump T p2 = p * p; \
	mediump T p3 = p * p2; \
	mediump T p5 = p2 * p3; \
	return clamp(taylor_normalization * (p - p3 * (1.0 / 6.0) + p5 * (1.0 / 120.0)), -1.0, 1.0); \
}
// No templates in GLSL. Stamp out macros.
gen_taylor(float)
gen_taylor(vec2)
gen_taylor(vec3)
gen_taylor(vec4)

// Given weights, compute a bilinear filter which implements the weight.
// All weights are known to be non-negative, and separable.
mediump vec3 compute_uv_phase_weight(mediump vec2 weights_u, mediump vec2 weights_v)
{
	// The sum of a bilinear sample has combined weight of 1, we will need to adjust the resulting sample
	// to match our actual weight sum.
	mediump float w = dot(weights_u.xyxy, weights_v.xxyy);
	mediump float x = weights_u.y / max(weights_u.x + weights_u.y, 0.001);
	mediump float y = weights_v.y / max(weights_v.x + weights_v.y, 0.001);
	return vec3(x, y, w);
}

BandlimitedPixelInfo compute_pixel_weights(vec2 uv, vec2 size, vec2 inv_size)
{
	// Get derivatives in texel space.
	// Need a non-zero derivative.
	vec2 extent = max(fwidth(uv) * size * (SMOOTHNESS + 0.5), 1.0 / 256.0);

	// Get base pixel and phase, range [0, 1).
	vec2 pixel = uv * size - 0.5;
	vec2 base_pixel = floor(pixel);
	vec2 phase = pixel - base_pixel;

	BandlimitedPixelInfo info;

	mediump vec2 inv_extent = 1.0 / extent;
	if (any(greaterThan(extent, vec2(maximum_support_extent))))
	{
		// We need to just do regular minimization filtering.
		info = BandlimitedPixelInfo(vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0),
		                            vec4(0.0, 0.0, 0.0, 0.0), 0.0);
	}
	else if (all(lessThanEqual(extent, vec2(0.5))))
	{
		// We can resolve the filter by just sampling a single 2x2 block.
		mediump vec2 shift = 0.5 + 0.5 * taylor_sin(PI_half * clamp(inv_extent * (phase - 0.5), -1.0, 1.0));
		info = BandlimitedPixelInfo((base_pixel + 0.5 + shift) * inv_size, vec2(0.0), vec2(0.0), vec2(0.0),
		                            vec4(1.0, 0.0, 0.0, 0.0), 1.0);
	}
	else
	{
		// Full 4x4 sampling.

		// Fade between bandlimited and normal sampling.
		// Fully use bandlimited filter at LOD 0, normal filtering at approx. LOD -0.5.
		mediump float max_extent = max(extent.x, extent.y);
		mediump float l = clamp(1.0 - (max_extent - 1.0) / (maximum_support_extent - 1.0), 0.0, 1.0);

		mediump vec4 sine_phases_x = PI_half * clamp(inv_extent.x * (phase.x + vec4(1.5, 0.5, -0.5, -1.5)), -1.0, 1.0);
		mediump vec4 sines_x = taylor_sin(sine_phases_x);

		mediump vec4 sine_phases_y = PI_half * clamp(inv_extent.y * (phase.y + vec4(1.5, 0.5, -0.5, -1.5)), -1.0, 1.0);
		mediump vec4 sines_y = taylor_sin(sine_phases_y);

		mediump vec2 sine_phases_end = PI_half * clamp(inv_extent * (phase - 2.5), -1.0, 1.0);
		mediump vec2 sines_end = taylor_sin(sine_phases_end);

		mediump vec4 weights_x = 0.5 * (sines_x - vec4(sines_x.yzw, sines_end.x));
		mediump vec4 weights_y = 0.5 * (sines_y - vec4(sines_y.yzw, sines_end.y));

		mediump vec3 w0 = compute_uv_phase_weight(weights_x.xy, weights_y.xy);
		mediump vec3 w1 = compute_uv_phase_weight(weights_x.zw, weights_y.xy);
		mediump vec3 w2 = compute_uv_phase_weight(weights_x.xy, weights_y.zw);
		mediump vec3 w3 = compute_uv_phase_weight(weights_x.zw, weights_y.zw);

		info = BandlimitedPixelInfo((base_pixel - 0.5 + w0.xy) * inv_size,
									(base_pixel + vec2(1.5, -0.5) + w1.xy) * inv_size,
									(base_pixel + vec2(-0.5, 1.5) + w2.xy) * inv_size,
									(base_pixel + 1.5 + w3.xy) * inv_size,
									vec4(w0.z, w1.z, w2.z, w3.z), l);
	}

	return info;
}

mediump vec4 sample_bandlimited_pixel(sampler2D samp, vec2 uv, BandlimitedPixelInfo info, float lod)
{
	mediump vec4 color = texture2D(samp, uv);
	if (info.l > 0.0)
	{
		mediump vec4 bandlimited = info.weights.x * pow(texture2D(samp, info.uv0, lod), vec4(2.2));
		if (info.weights.x < 1.0)
		{
			bandlimited += info.weights.y * pow(texture2D(samp, info.uv1, lod), vec4(2.2));
			bandlimited += info.weights.z * pow(texture2D(samp, info.uv2, lod), vec4(2.2));
			bandlimited += info.weights.w * pow(texture2D(samp, info.uv3, lod), vec4(2.2));
		}
		color = mix(color, bandlimited, info.l);
	}
	return color;
}

void main()
{
	BandlimitedPixelInfo info = compute_pixel_weights(texcoord, texsize.xy, 1.0 / texsize.xy);
	mediump vec3 result = sample_bandlimited_pixel(textures[0], texcoord, info, 0.0).rgb;
	gl_FragColor = vec4(sqrt(clamp(result, 0.0, 1.0)), 1.0);
}