// xBRZ freescale
// based on :

/*
   Hyllian's xBR-vertex code and texel mapping
   
   Copyright (C) 2011/2016 Hyllian - sergiogdb@gmail.com

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is 
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.

*/ 

// This shader also uses code and/or concepts from xBRZ as it appears
// in the Desmume source code. The license for which is as follows:

// ****************************************************************************
// * This file is part of the HqMAME project. It is distributed under         *
// * GNU General Public License: http://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
// *                                                                          *
// * Additionally and as a special exception, the author gives permission     *
// * to link the code of this program with the MAME library (or with modified *
// * versions of MAME that use the same license as MAME), and distribute      *
// * linked combinations including the two. You must obey the GNU General     *
// * Public License in all respects for all of the code used other than MAME. *
// * If you modify this file, you may extend this exception to your version   *
// * of the file, but you are not obligated to do so. If you do not wish to   *
// * do so, delete this exception statement from your version.                *
// ****************************************************************************
// ported to blastem shader format by hunterk

uniform sampler2D textures[2];
uniform mediump vec2 texsize;
varying mediump vec2 texcoord;

#define BLEND_NONE 0
#define BLEND_NORMAL 1
#define BLEND_DOMINANT 2
#define LUMINANCE_WEIGHT 1.0
#define EQUAL_COLOR_TOLERANCE 30.0/255.0
#define STEEP_DIRECTION_THRESHOLD 2.2
#define DOMINANT_DIRECTION_THRESHOLD 3.6

// this is usually handled automatically but blastem doesn't expose output size?
#define scale vec2(10.0, 10.0)

mediump float DistYCbCr(mediump vec3 pixA, mediump vec3 pixB)
{
  const mediump vec3 w = vec3(0.2627, 0.6780, 0.0593);
  const mediump float scaleB = 0.5 / (1.0 - w.b);
  const mediump float scaleR = 0.5 / (1.0 - w.r);
  mediump vec3 diff = pixA - pixB;
  mediump float Y = dot(diff.rgb, w);
  mediump float Cb = scaleB * (diff.b - Y);
  mediump float Cr = scaleR * (diff.r - Y);

  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));
}

bool IsPixEqual(const mediump vec3 pixA, const mediump vec3 pixB)
{
  return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);
}

mediump float get_left_ratio(mediump vec2 center, mediump vec2 origin, mediump vec2 direction, mediump vec2 scale_)
{
  mediump vec2 P0 = center - origin;
  mediump vec2 proj = direction * (dot(P0, direction) / dot(direction, direction));
  mediump vec2 distv = P0 - proj;
  mediump vec2 orth = vec2(-direction.y, direction.x);
  mediump float side = sign(dot(P0, orth));
  mediump float v = side * length(distv * scale_);

//  return step(0, v);
  return smoothstep(-sqrt(2.0)/2.0, sqrt(2.0)/2.0, v);
}

#define eq(a,b)  (a == b)
#define neq(a,b) (a != b)

#define P(x,y) texture2D(textures[0], coord + (1.0 / texsize) * vec2(x, y)).rgb

void main()
{
  //---------------------------------------
  // Input Pixel Mapping:  -|x|x|x|-
  //                       x|A|B|C|x
  //                       x|D|E|F|x
  //                       x|G|H|I|x
  //                       -|x|x|x|-

  mediump vec2 pos = fract(texcoord * texsize.xy) - vec2(0.5, 0.5);
  mediump vec2 coord = texcoord - pos * (1.0 / texsize.xy);
  
  mediump vec3 A = P(-1,-1);
  mediump vec3 B = P( 0,-1);
  mediump vec3 C = P( 1,-1);
  mediump vec3 D = P(-1, 0);
  mediump vec3 E = P( 0, 0);
  mediump vec3 F = P( 1, 0);
  mediump vec3 G = P(-1, 1);
  mediump vec3 H = P( 0, 1);
  mediump vec3 I = P( 1, 1);

  // blendResult Mapping: x|y|
  //                      w|z|
  ivec4 blendResult = ivec4(BLEND_NONE,BLEND_NONE,BLEND_NONE,BLEND_NONE);

  // Preprocess corners
  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|-|B|C|-
  //                    -|D|E|F|x
  //                    -|G|H|I|x
  //                    -|-|x|x|-
  if (!((eq(E,F) && eq(H,I)) || (eq(E,H) && eq(F,I))))
  {
    mediump float dist_H_F = DistYCbCr(G, E) + DistYCbCr(E, C) + DistYCbCr(P(0,2), I) + DistYCbCr(I, P(2,0)) + (4.0 * DistYCbCr(H, F));
    mediump float dist_E_I = DistYCbCr(D, H) + DistYCbCr(H, P(1,2)) + DistYCbCr(B, F) + DistYCbCr(F, P(2,1)) + (4.0 * DistYCbCr(E, I));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_H_F) < dist_E_I;
    blendResult.z = ((dist_H_F < dist_E_I) && neq(E,F) && neq(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }


  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|A|B|-|-
  //                    x|D|E|F|-
  //                    x|G|H|I|-
  //                    -|x|x|-|-
  if (!((eq(D,E) && eq(G,H)) || (eq(D,G) && eq(E,H))))
  {
    mediump float dist_G_E = DistYCbCr(P(-2,1)  , D) + DistYCbCr(D, B) + DistYCbCr(P(-1,2), H) + DistYCbCr(H, F) + (4.0 * DistYCbCr(G, E));
    mediump float dist_D_H = DistYCbCr(P(-2,0)  , G) + DistYCbCr(G, P(0,2)) + DistYCbCr(A, E) + DistYCbCr(E, I) + (4.0 * DistYCbCr(D, H));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_H) < dist_G_E;
    blendResult.w = ((dist_G_E > dist_D_H) && neq(E,D) && neq(E,H)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Pixel Tap Mapping: -|-|x|x|-
  //                    -|A|B|C|x
  //                    -|D|E|F|x
  //                    -|-|H|I|-
  //                    -|-|-|-|-
  if (!((eq(B,C) && eq(E,F)) || (eq(B,E) && eq(C,F))))
  {
    mediump float dist_E_C = DistYCbCr(D, B) + DistYCbCr(B, P(1,-2)) + DistYCbCr(H, F) + DistYCbCr(F, P(2,-1)) + (4.0 * DistYCbCr(E, C));
    mediump float dist_B_F = DistYCbCr(A, E) + DistYCbCr(E, I) + DistYCbCr(P(0,-2), C) + DistYCbCr(C, P(2,0)) + (4.0 * DistYCbCr(B, F));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_B_F) < dist_E_C;
    blendResult.y = ((dist_E_C > dist_B_F) && neq(E,B) && neq(E,F)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Pixel Tap Mapping: -|x|x|-|-
  //                    x|A|B|C|-
  //                    x|D|E|F|-
  //                    -|G|H|-|-
  //                    -|-|-|-|-
  if (!((eq(A,B) && eq(D,E)) || (eq(A,D) && eq(B,E))))
  {
    mediump float dist_D_B = DistYCbCr(P(-2,0), A) + DistYCbCr(A, P(0,-2)) + DistYCbCr(G, E) + DistYCbCr(E, C) + (4.0 * DistYCbCr(D, B));
    mediump float dist_A_E = DistYCbCr(P(-2,-1), D) + DistYCbCr(D, H) + DistYCbCr(P(-1,-2), B) + DistYCbCr(B, F) + (4.0 * DistYCbCr(A, E));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_B) < dist_A_E;
    blendResult.x = ((dist_D_B < dist_A_E) && neq(E,D) && neq(E,B)) ? ((dominantGradient) ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  mediump vec3 res = E;

  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|-|B|C|-
  //                    -|D|E|F|x
  //                    -|G|H|I|x
  //                    -|-|x|x|-
  if(blendResult.z != BLEND_NONE)
  {
    mediump float dist_F_G = DistYCbCr(F, G);
    mediump float dist_H_C = DistYCbCr(H, C);
    bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||
                !((blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) ||
                  (IsPixEqual(G, H) && IsPixEqual(H, I) && IsPixEqual(I, F) && IsPixEqual(F, C) && !IsPixEqual(E, I))));

    mediump vec2 origin = vec2(0.0, 1.0 / sqrt(2.0));
    mediump vec2 direction = vec2(1.0, -1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && neq(E,G) && neq(D,G);
      bool haveSteepLine = (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && neq(E,C) && neq(B,C);
      origin = haveShallowLine? vec2(0.0, 0.25) : vec2(0.0, 0.5);
      direction.x += haveShallowLine? 1.0: 0.0;
      direction.y -= haveSteepLine? 1.0: 0.0;
    }

    mediump vec3 blendPix = mix(H,F, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|-|-|-|-
  //                    -|A|B|-|-
  //                    x|D|E|F|-
  //                    x|G|H|I|-
  //                    -|x|x|-|-
  if(blendResult.w != BLEND_NONE)
  {
    mediump float dist_H_A = DistYCbCr(H, A);
    mediump float dist_D_I = DistYCbCr(D, I);
    bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||
                !((blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) ||
                  (IsPixEqual(A, D) && IsPixEqual(D, G) && IsPixEqual(G, H) && IsPixEqual(H, I) && !IsPixEqual(E, G))));

    mediump vec2 origin = vec2(-1.0 / sqrt(2.0), 0.0);
    mediump vec2 direction = vec2(1.0, 1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && neq(E,A) && neq(B,A);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && neq(E,I) && neq(F,I);
      origin = haveShallowLine? vec2(-0.25, 0.0) : vec2(-0.5, 0.0);
      direction.y += haveShallowLine? 1.0: 0.0;
      direction.x += haveSteepLine? 1.0: 0.0;
    }
    origin = origin;
    direction = direction;

    mediump vec3 blendPix = mix(H,D, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|-|x|x|-
  //                    -|A|B|C|x
  //                    -|D|E|F|x
  //                    -|-|H|I|-
  //                    -|-|-|-|-
  if(blendResult.y != BLEND_NONE)
  {
    mediump float dist_B_I = DistYCbCr(B, I);
    mediump float dist_F_A = DistYCbCr(F, A);
    bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||
                !((blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) ||
                  (IsPixEqual(I, F) && IsPixEqual(F, C) && IsPixEqual(C, B) && IsPixEqual(B, A) && !IsPixEqual(E, C))));

    mediump vec2 origin = vec2(1.0 / sqrt(2.0), 0.0);
    mediump vec2 direction = vec2(-1.0, -1.0);

    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && neq(E,I) && neq(H,I);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && neq(E,A) && neq(D,A);
      origin = haveShallowLine? vec2(0.25, 0.0) : vec2(0.5, 0.0);
      direction.y -= haveShallowLine? 1.0: 0.0;
      direction.x -= haveSteepLine? 1.0: 0.0;
    }

    mediump vec3 blendPix = mix(F,B, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

  // Pixel Tap Mapping: -|x|x|-|-
  //                    x|A|B|C|-
  //                    x|D|E|F|-
  //                    -|G|H|-|-
  //                    -|-|-|-|-
  if(blendResult.x != BLEND_NONE)
  {
    mediump float dist_D_C = DistYCbCr(D, C);
    mediump float dist_B_G = DistYCbCr(B, G);
    bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||
                !((blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) ||
                  (IsPixEqual(C, B) && IsPixEqual(B, A) && IsPixEqual(A, D) && IsPixEqual(D, G) && !IsPixEqual(E, A))));

    mediump vec2 origin = vec2(0.0, -1.0 / sqrt(2.0));
    mediump vec2 direction = vec2(-1.0, 1.0);
    if(doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && neq(E,C) && neq(F,C);
      bool haveSteepLine  = (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && neq(E,G) && neq(H,G);
      origin = haveShallowLine? vec2(0.0, -0.25) : vec2(0.0, -0.5);
      direction.x -= haveShallowLine? 1.0: 0.0;
      direction.y += haveSteepLine? 1.0: 0.0;
    }

    mediump vec3 blendPix = mix(D,B, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    res = mix(res, blendPix, get_left_ratio(pos, origin, direction, scale));
  }

 	gl_FragColor = vec4(res, 1.0);
}