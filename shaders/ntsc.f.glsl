//******************************************************************************
// NTSC composite simulator for BlastEm
// Shader by Sik, based on BlastEm's default shader
//
// It works by converting from RGB to YIQ and then encoding it into NTSC, then
// trying to decode it back. The lossy nature of the encoding process results in
// the rainbow effect. It also accounts for the differences between H40 and H32
// mode as it computes the exact colorburst cycle length.
//
// This shader tries to work around the inability to keep track of previous
// pixels by sampling seven points (in 0.25 colorburst cycle intervals), that
// seems to be enough to give decent filtering (four samples are used for
// low-pass filtering, but we need seven because decoding chroma also requires
// four samples so we're filtering over overlapping samples... just see the
// comments in the I/Q code to understand).
//******************************************************************************

uniform mediump float width;
uniform sampler2D textures[2];
uniform mediump vec2 texsize;
varying mediump vec2 texcoord;

// Converts from RGB to YIQ
mediump vec3 rgba2yiq(vec4 rgba)
{
	return vec3(
		rgba[0] * 0.3 + rgba[1] * 0.59 + rgba[2] * 0.11,
		rgba[0] * 0.599 + rgba[1] * -0.2773 + rgba[2] * -0.3217,
		rgba[0] * 0.213 + rgba[1] * -0.5251 + rgba[2] * 0.3121
	);
}

// Encodes YIQ into composite
mediump float yiq2raw(vec3 yiq, float phase)
{
	return yiq[0] + yiq[1] * sin(phase) + yiq[2] * cos(phase);
}

// Converts from YIQ to RGB
mediump vec4 yiq2rgba(vec3 yiq)
{
	return vec4(
		yiq[0] + yiq[1] * 0.9469 + yiq[2] * 0.6236,
		yiq[0] - yiq[1] * 0.2748 - yiq[2] * 0.6357,
		yiq[0] - yiq[1] * 1.1 + yiq[2] * 1.7,
		1.0
	);
}

void main()
{
	// Use first pair of lines for hard line edges
	// Use second pair of lines for soft line edges
	mediump float modifiedY0 = (floor(texcoord.y * texsize.y + 0.25) + 0.5) / texsize.y;
	mediump float modifiedY1 = (floor(texcoord.y * texsize.y - 0.25) + 0.5) / texsize.y;
	//mediump float modifiedY0 = (texcoord.y * texsize.y + 0.75) / texsize.y;
	//mediump float modifiedY1 = (texcoord.y * texsize.y + 0.25) / texsize.y;
	
	// Used by the mixing when fetching texels, related to the way BlastEm
	// handles interlaced mode (nothing to do with composite)
	mediump float factorY = (sin(texcoord.y * texsize.y * 6.283185307) + 1.0) * 0.5;
	
	// Horizontal distance of half a colorburst cycle
	mediump float factorX = (1.0 / texsize.x) / 170.667 * 0.5 * (width - 27.0);
	
	// Where we store the sampled pixels.
	// [0] = current pixel
	// [1] = 1/4 colorburst cycles earlier
	// [2] = 2/4 colorburst cycles earlier
	// [3] = 3/4 colorburst cycles earlier
	// [4] = 1 colorburst cycle earlier
	// [5] = 1 1/4 colorburst cycles earlier
	// [6] = 1 2/4 colorburst cycles earlier
	mediump float phase[7];		// Colorburst phase (in radians)
	mediump float raw[7];		// Raw encoded composite signal
	
	// Sample all the pixels we're going to use
	mediump float x = texcoord.x;
	for (int n = 0; n < 7; n++, x -= factorX * 0.5) {
		// Compute colorburst phase at this point
		phase[n] = x / factorX * 3.1415926;
		
		// Decode RGB into YIQ and then into composite
		// Reading two textures is a BlastEm thing :P (the two fields in
		// interlaced mode, that's taken as-is from the stock shaders)
		raw[n] = yiq2raw(mix(
			rgba2yiq(texture2D(textures[1], vec2(x, modifiedY1))),
			rgba2yiq(texture2D(textures[0], vec2(x, modifiedY0))),
			factorY
		), phase[n]);
	}
	
	// Decode Y by averaging over the the whole sampled cycle (effectively
	// filtering anything above the colorburst frequency)
	mediump float y_mix = (raw[0] + raw[1] + raw[2] + raw[3]) * 0.25;
	
	// Decode I and Q (see page below to understand what's going on)
	// https://codeandlife.com/2012/10/09/composite-video-decoding-theory-and-practice/
	//
	// Retrieving I and Q out of the raw signal is done like this
	// (use sin for I and cos for Q):
	//
	//    0.5 * raw[0] * sin(phase[0]) + 0.5 * raw[1] * sin(phase[1]) +
	//    0.5 * raw[2] * sin(phase[2]) + 0.5 * raw[3] * sin(phase[3])
	//
	// i.e. multiply each of the sampled quarter cycles against the reference
	// wave and average them (actually double that because for some reason
	// that's needed to get the correct scale, hence 0.5 instead of 0.25)
	//
	// That turns out to be blocky tho, so we opt to filter down the chroma...
	// which requires doing the above *four* times if we do it the same way as
	// we did for luminance (note that 0.125 = 1/4 of 0.5):
	//
	//    0.125 * raw[0] * sin(phase[0]) + 0.125 * raw[1] * sin(phase[1]) +
	//    0.125 * raw[2] * sin(phase[2]) + 0.125 * raw[3] * sin(phase[3]) +
	//    0.125 * raw[1] * sin(phase[1]) + 0.125 * raw[2] * sin(phase[2]) +
	//    0.125 * raw[3] * sin(phase[3]) + 0.125 * raw[4] * sin(phase[4]) +
	//    0.125 * raw[2] * sin(phase[2]) + 0.125 * raw[3] * sin(phase[3]) +
	//    0.125 * raw[4] * sin(phase[4]) + 0.125 * raw[5] * sin(phase[5]) +
	//    0.125 * raw[3] * sin(phase[3]) + 0.125 * raw[4] * sin(phase[4]) +
	//    0.125 * raw[5] * sin(phase[5]) + 0.125 * raw[6] * sin(phase[6])
	//
	// There are a lot of repeated values there that could be merged into one,
	// what you see below is the resulting simplification.
	
	mediump float i_mix =
		0.125 * raw[0] * sin(phase[0]) +
		0.25  * raw[1] * sin(phase[1]) +
		0.375 * raw[2] * sin(phase[2]) +
		0.5   * raw[3] * sin(phase[3]) +
		0.375 * raw[4] * sin(phase[4]) +
		0.25  * raw[5] * sin(phase[5]) +
		0.125 * raw[6] * sin(phase[6]);
	
	mediump float q_mix =
		0.125 * raw[0] * cos(phase[0]) +
		0.25  * raw[1] * cos(phase[1]) +
		0.375 * raw[2] * cos(phase[2]) +
		0.5   * raw[3] * cos(phase[3]) +
		0.375 * raw[4] * cos(phase[4]) +
		0.25  * raw[5] * cos(phase[5]) +
		0.125 * raw[6] * cos(phase[6]);
	
	// Convert YIQ back to RGB and output it
	gl_FragColor = yiq2rgba(vec3(y_mix, i_mix, q_mix));
	
	// If you're curious to see what the raw composite signal looks like,
	// comment out the above and uncomment the line below instead
	//gl_FragColor = vec4(raw[0], raw[0], raw[0], 1.0);
}
