
attribute vec2 pos;
varying mediump vec2 texcoord;
uniform mediump float width, height;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = sign(pos) * vec2(0.5 * width, -0.5 * height) + vec2(0.5 * width, 0.5 * height);
}
