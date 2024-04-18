
attribute vec2 pos;
attribute vec2 uv;
varying mediump vec2 texcoord;
uniform mediump float width, height;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = uv;
}
