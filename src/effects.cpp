#include "effects.hpp"

#include <algorithm>
#include <cctype>

namespace gh::fx {

namespace {

// The screen effects of JS Engine's Lua functions (addVCREffect, addBlockedGlitchEffect...),
// with the GLSL of its Shaders.hx (Apache License 2.0) as it is: they look the same.
// The Flixel-style GLSL goes through translateShader like any mod shader.

// ChromaticAberrationShader
constexpr const char* kChromatic = R"GLSL(#pragma header

uniform float rOffset;
uniform float gOffset;
uniform float bOffset;

void main()
{
    vec4 col1 = texture2D(bitmap, openfl_TextureCoordv.st - vec2(rOffset, 0.0));
    vec4 col2 = texture2D(bitmap, openfl_TextureCoordv.st - vec2(gOffset, 0.0));
    vec4 col3 = texture2D(bitmap, openfl_TextureCoordv.st - vec2(bOffset, 0.0));
    vec4 toUse = texture2D(bitmap, openfl_TextureCoordv);
    toUse.r = col1.r;
    toUse.g = col2.g;
    toUse.b = col3.b;

    gl_FragColor = toUse;
}
)GLSL";

// Scanline
constexpr const char* kScanline = R"GLSL(    #pragma header
    const float scale = 1.0;
uniform bool lockAlpha;
    void main()
    {
        if (mod(floor(openfl_TextureCoordv.y * openfl_TextureSize.y / scale), 2.0) == 0.0 ){
            float bitch = 1.0;

            vec4 texColor = texture2D(bitmap, openfl_TextureCoordv);
            if (lockAlpha) bitch = texColor.a;
            gl_FragColor = vec4(0.0, 0.0, 0.0, bitch);
        }else{
            gl_FragColor = texture2D(bitmap, openfl_TextureCoordv);
        }
    }
)GLSL";

// Grain (Martins Upitis' film grain, CC BY 3.0); two globals read from a uniform are #defines here,
// as GLSL ES and core profiles only take constant global initialisers
constexpr const char* kGrain = R"GLSL(    #pragma header

    /*
    Film Grain post-process shader v1.1
    Martins Upitis (martinsh) devlog-martinsh.blogspot.com
    2013

    --------------------------
    This work is licensed under a Creative Commons Attribution 3.0 Unported License.
    So you are free to share, modify and adapt it for your needs, and even use it for commercial use.
    I would also love to hear about a project you are using it.

    Have fun,
    Martins
    --------------------------

    Perlin noise shader by toneburst:
    http://machinesdontcare.wordpress.com/2009/06/25/3d-perlin-noise-sphere-vertex-shader-sourcecode/
    */
    uniform float uTime;

    const float permTexUnit = 1.0/256.0;        // Perm texture texel-size
    const float permTexUnitHalf = 0.5/256.0;    // Half perm texture texel-size

    #define width openfl_TextureSize.x  // was a global set from a uniform
    #define height openfl_TextureSize.y

    const float grainamount = 0.05; //grain amount
    bool colored = false; //colored noise?
    uniform float coloramount;
    uniform float grainsize; //grain particle size (1.5 - 2.5)
    uniform float lumamount; //
uniform bool lockAlpha;

    //a random texture generator, but you can also use a pre-computed perturbation texture

    vec4 rnm(in vec2 tc)
    {
        float noise =  sin(dot(tc + vec2(uTime,uTime),vec2(12.9898,78.233))) * 43758.5453;

        float noiseR =  fract(noise)*2.0-1.0;
        float noiseG =  fract(noise*1.2154)*2.0-1.0;
        float noiseB =  fract(noise * 1.3453) * 2.0 - 1.0;

        float noiseA =  (fract(noise * 1.3647) * 2.0 - 1.0);

        return vec4(noiseR,noiseG,noiseB,noiseA);
    }

    float fade(in float t) {
        return t*t*t*(t*(t*6.0-15.0)+10.0);
    }

    float pnoise3D(in vec3 p)
    {
        vec3 pi = permTexUnit*floor(p)+permTexUnitHalf; // Integer part, scaled so +1 moves permTexUnit texel
        // and offset 1/2 texel to sample texel centers
        vec3 pf = fract(p);     // Fractional part for interpolation

        // Noise contributions from (x=0, y=0), z=0 and z=1
        float perm00 = rnm(pi.xy).a ;
        vec3  grad000 = rnm(vec2(perm00, pi.z)).rgb * 4.0 - 1.0;
        float n000 = dot(grad000, pf);
        vec3  grad001 = rnm(vec2(perm00, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
        float n001 = dot(grad001, pf - vec3(0.0, 0.0, 1.0));

        // Noise contributions from (x=0, y=1), z=0 and z=1
        float perm01 = rnm(pi.xy + vec2(0.0, permTexUnit)).a ;
        vec3  grad010 = rnm(vec2(perm01, pi.z)).rgb * 4.0 - 1.0;
        float n010 = dot(grad010, pf - vec3(0.0, 1.0, 0.0));
        vec3  grad011 = rnm(vec2(perm01, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
        float n011 = dot(grad011, pf - vec3(0.0, 1.0, 1.0));

        // Noise contributions from (x=1, y=0), z=0 and z=1
        float perm10 = rnm(pi.xy + vec2(permTexUnit, 0.0)).a ;
        vec3  grad100 = rnm(vec2(perm10, pi.z)).rgb * 4.0 - 1.0;
        float n100 = dot(grad100, pf - vec3(1.0, 0.0, 0.0));
        vec3  grad101 = rnm(vec2(perm10, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
        float n101 = dot(grad101, pf - vec3(1.0, 0.0, 1.0));

        // Noise contributions from (x=1, y=1), z=0 and z=1
        float perm11 = rnm(pi.xy + vec2(permTexUnit, permTexUnit)).a ;
        vec3  grad110 = rnm(vec2(perm11, pi.z)).rgb * 4.0 - 1.0;
        float n110 = dot(grad110, pf - vec3(1.0, 1.0, 0.0));
        vec3  grad111 = rnm(vec2(perm11, pi.z + permTexUnit)).rgb * 4.0 - 1.0;
        float n111 = dot(grad111, pf - vec3(1.0, 1.0, 1.0));

        // Blend contributions along x
        vec4 n_x = mix(vec4(n000, n001, n010, n011), vec4(n100, n101, n110, n111), fade(pf.x));

        // Blend contributions along y
        vec2 n_xy = mix(n_x.xy, n_x.zw, fade(pf.y));

        // Blend contributions along z
        float n_xyz = mix(n_xy.x, n_xy.y, fade(pf.z));

        // We are done, return the final noise value.
        return n_xyz;
    }

    //2d coordinate orientation thing
    vec2 coordRot(in vec2 tc, in float angle)
    {
        float aspect = width/height;
        float rotX = ((tc.x*2.0-1.0)*aspect*cos(angle)) - ((tc.y*2.0-1.0)*sin(angle));
        float rotY = ((tc.y*2.0-1.0)*cos(angle)) + ((tc.x*2.0-1.0)*aspect*sin(angle));
        rotX = ((rotX/aspect)*0.5+0.5);
        rotY = rotY*0.5+0.5;
        return vec2(rotX,rotY);
    }

    void main()
    {
        vec2 texCoord = openfl_TextureCoordv.st;

        vec3 rotOffset = vec3(1.425,3.892,5.835); //rotation offset values
        vec2 rotCoordsR = coordRot(texCoord, uTime + rotOffset.x);
        vec3 noise = vec3(pnoise3D(vec3(rotCoordsR*vec2(width/grainsize,height/grainsize),0.0)));

        if (colored)
        {
            vec2 rotCoordsG = coordRot(texCoord, uTime + rotOffset.y);
            vec2 rotCoordsB = coordRot(texCoord, uTime + rotOffset.z);
            noise.g = mix(noise.r,pnoise3D(vec3(rotCoordsG*vec2(width/grainsize,height/grainsize),1.0)),coloramount);
            noise.b = mix(noise.r,pnoise3D(vec3(rotCoordsB*vec2(width/grainsize,height/grainsize),2.0)),coloramount);
        }

        vec3 col = texture2D(bitmap, openfl_TextureCoordv).rgb;

        //noisiness response curve based on scene luminance
        vec3 lumcoeff = vec3(0.299,0.587,0.114);
        float luminance = mix(0.0,dot(col, lumcoeff),lumamount);
        float lum = smoothstep(0.2,0.0,luminance);
        lum += luminance;

        noise = mix(noise,vec3(0.0),pow(lum,4.0));
        col = col+noise*grainamount;

            float bitch = 1.0;
        vec4 texColor = texture2D(bitmap, openfl_TextureCoordv);
            if (lockAlpha) bitch = texColor.a;
        gl_FragColor =  vec4(col,bitch);
    }
)GLSL";

// Tiltshift (Martin Jonasson's tilt shift, MIT)
constexpr const char* kTiltshift = R"GLSL(#pragma header

// Modified version of a tilt shift shader from Martin Jonasson (http://grapefrukt.com/)
// Read http://notes.underscorediscovery.com/ for context on shaders and this file
// License : MIT

    /*
        Take note that blurring in a single pass (the two for loops below) is more expensive than separating
        the x and the y blur into different passes. This was used where bleeding edge performance
        was not crucial and is to illustrate a point.

        The reason two passes is cheaper?
           texture2D is a fairly high cost call, sampling a texture.

           So, in a single pass, like below, there are 3 steps, per x and y.

           That means a total of 9 "taps", it touches the texture to sample 9 times.

           Now imagine we apply this to some geometry, that is equal to 16 pixels on screen (tiny)
           (16 * 16) * 9 = 2304 samples taken, for width * height number of pixels, * 9 taps
           Now, if you split them up, it becomes 3 for x, and 3 for y, a total of 6 taps
           (16 * 16) * 6 = 1536 samples

           That's on a *tiny* sprite, let's scale that up to 128x128 sprite...
           (128 * 128) * 9 = 147,456
           (128 * 128) * 6 =  98,304

           That's 33.33..% cheaper for splitting them up.
           That's with 3 steps, with higher steps (more taps per pass...)

           A really smooth, 6 steps, 6*6 = 36 taps for one pass, 12 taps for two pass
           You will notice, the curve is not linear, at 12 steps it's 144 vs 24 taps
           It becomes orders of magnitude slower to do single pass!
           Therefore, you split them up into two passes, one for x, one for y.
    */

// I am hardcoding the constants like a jerk

uniform float bluramount;
uniform float center;
const float stepSize    = 0.004;
const float steps       = 3.0;

const float minOffs     = (float(steps-1.0)) / -2.0;
const float maxOffs     = (float(steps-1.0)) / +2.0;

void main() {
    float amount;
    vec4 blurred;

    // Work out how much to blur based on the mid point
    amount = pow((openfl_TextureCoordv.y * center) * 2.0 - 1.0, 2.0) * bluramount;

    // This is the accumulation of color from the surrounding pixels in the texture
    blurred = vec4(0.0, 0.0, 0.0, 1.0);

    // From minimum offset to maximum offset
    for (float offsX = minOffs; offsX <= maxOffs; ++offsX) {
        for (float offsY = minOffs; offsY <= maxOffs; ++offsY) {

            // copy the coord so we can mess with it
            vec2 temp_tcoord = openfl_TextureCoordv.xy;

            //work out which uv we want to sample now
            temp_tcoord.x += offsX * amount * stepSize;
            temp_tcoord.y += offsY * amount * stepSize;

            // accumulate the sample
            blurred += texture2D(bitmap, temp_tcoord);
        }
    }

    // because we are doing an average, we divide by the amount (x AND y, hence steps * steps)
    blurred /= float(steps * steps);

    // return the final blurred color
    gl_FragColor = blurred;
}
)GLSL";

// VCRDistortionShader
constexpr const char* kVcr = R"GLSL(#pragma header

uniform float iTime;
uniform bool vignetteOn;
uniform bool perspectiveOn;
uniform bool distortionOn;
uniform bool scanlinesOn;
uniform bool vignetteMoving;
uniform float glitchModifier;
uniform vec3 iResolution;

float onOff(float a, float b, float c)
{
    return step(c, sin(iTime + a*cos(iTime*b)));
}

float ramp(float y, float start, float end)
{
    float inside = step(start,y) - step(end,y);
    float fact = (y-start)/(end-start)*inside;
    // FIX: 1. -> 1.0
    return (1.0-fact) * inside;
}

vec4 getVideo(vec2 uv)
{
    vec2 look = uv;
    if(distortionOn){
        // FIX: 1./(1.+20.*... -> 1.0/(1.0+20.0*..., mod(iTime/4.,1.) -> mod(iTime/4.0,1.0)
        float window = 1.0/(1.0+20.0*(look.y-mod(iTime/4.0,1.0))*(look.y-mod(iTime/4.0,1.0)));
        // FIX: 10. -> 10.0, /50. -> /50.0, onOff(4.,4.,.3) -> onOff(4.0,4.0,0.3), 1.+ -> 1.0+, *2) -> *2.0)
        look.x = look.x + (sin(look.y*10.0 + iTime)/50.0*onOff(4.0,4.0,0.3)*(1.0+cos(iTime*80.0))*window)*(glitchModifier*2.0);
        // FIX: onOff(2.,3.,.9) -> onOff(2.0,3.0,0.9)
        float vShift = 0.4*onOff(2.0,3.0,0.9)*(sin(iTime)*sin(iTime*20.0) +
                                         (0.5 + 0.1*sin(iTime*200.0)*cos(iTime)));
        // FIX: mod(..., 1.) -> mod(..., 1.0)
        look.y = mod(look.y + vShift*glitchModifier, 1.0);
    }
    vec4 video = flixel_texture2D(bitmap,look);
    return video;
}

vec2 screenDistort(vec2 uv)
{
  if(perspectiveOn){
    uv = (uv - 0.5) * 2.0;
      uv *= 1.1;
      uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
      uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
      uv  = (uv / 2.0) + 0.5;
      uv =  uv *0.92 + 0.04;
      return uv;
  }
    return uv;
}

float random(vec2 uv)
{
     return fract(sin(dot(uv, vec2(15.5151, 42.2561))) * 12341.14122 * sin(iTime * 0.03));
}

float noise(vec2 uv)
{
     vec2 i = floor(uv);
    vec2 f = fract(uv);

    float a = random(i);
    // FIX: vec2(1.,0.) -> vec2(1.0,0.0), vec2(0.,1.) -> vec2(0.0,1.0), vec2(1.) -> vec2(1.0)
    float b = random(i + vec2(1.0, 0.0));
    float c = random(i + vec2(0.0, 1.0));
    float d = random(i + vec2(1.0));

    // FIX: smoothstep(0.,1.,f) -> smoothstep(0.0,1.0,f)
    vec2 u = smoothstep(0.0, 1.0, f);

    return mix(a,b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

vec2 scandistort(vec2 uv) {
    float scan1 = clamp(cos(uv.y * 2.0 + iTime), 0.0, 1.0);
    float scan2 = clamp(cos(uv.y * 2.0 + iTime + 4.0) * 10.0, 0.0, 1.0);
    float amount = scan1 * scan2 * uv.x;
    return uv;
}

void main()
{
    vec2 uv = openfl_TextureCoordv;
  vec2 curUV = screenDistort(uv);
    uv = scandistort(curUV);
    vec4 video = getVideo(uv);
  float vigAmt = 1.0;
  // FIX: 0. -> 0.0
  float x = 0.0;

  video.r = getVideo(vec2(x+uv.x+0.001,uv.y+0.001)).x+0.05;
  video.g = getVideo(vec2(x+uv.x+0.000,uv.y-0.002)).y+0.05;
  video.b = getVideo(vec2(x+uv.x-0.002,uv.y+0.000)).z+0.05;
  video.r += 0.08*getVideo(0.75*vec2(x+0.025, -0.027)+vec2(uv.x+0.001,uv.y+0.001)).x;
  // FIX: x+-0.022 -> x-0.022 (invalid operator combination)
  video.g += 0.05*getVideo(0.75*vec2(x-0.022, -0.02)+vec2(uv.x+0.000,uv.y-0.002)).y;
  // FIX: x+-0.02 -> x-0.02 (invalid operator combination)
  video.b += 0.08*getVideo(0.75*vec2(x-0.02, -0.018)+vec2(uv.x-0.002,uv.y+0.000)).z;

  video = clamp(video*0.6+0.4*video*video*1.0,0.0,1.0);
  if(vignetteMoving)
      // FIX: 3.+ -> 3.0+, .3* -> 0.3*, 5.* -> 5.0*, iTime*5. -> iTime*5.0
      vigAmt = 3.0+0.3*sin(iTime + 5.0*cos(iTime*5.0));

    // FIX: 1.- -> 1.0-, .5 -> 0.5
    float vignette = (1.0-vigAmt*(uv.y-0.5)*(uv.y-0.5))*(1.0-vigAmt*(uv.x-0.5)*(uv.x-0.5));

  if(vignetteOn)
     video *= vignette;

  // FIX: 75. -> 75.0, .05 -> 0.05
  gl_FragColor = mix(video, vec4(noise(uv * 75.0)), 0.05);

  // FIX: <0 -> <0.0, >1 -> >1.0 (integer comparisons with float)
  if(curUV.x<0.0 || curUV.x>1.0 || curUV.y<0.0 || curUV.y>1.0){
    // FIX: vec4(0,0,0,0) -> vec4(0.0,0.0,0.0,0.0) (integer args in vector constructor)
    gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
  }
}
)GLSL";

// WiggleShader (WiggleEffect.hx)
constexpr const char* kWiggle = R"GLSL(#pragma header
//uniform float tx, ty; // x,y waves phase
uniform float uTime;

const int EFFECT_TYPE_DREAMY = 0;
const int EFFECT_TYPE_WAVY = 1;
const int EFFECT_TYPE_HEAT_WAVE_HORIZONTAL = 2;
const int EFFECT_TYPE_HEAT_WAVE_VERTICAL = 3;
const int EFFECT_TYPE_FLAG = 4;
const int EFFECT_TYPE_HEAT_WAVE_BOTH = 5;

uniform int effectType;

/**
 * How fast the waves move over time
 */
uniform float uSpeed;

/**
 * Number of waves over time
 */
uniform float uFrequency;

/**
 * How much the pixels are going to stretch over the waves
 */
uniform float uWaveAmplitude;

uniform float verticalStrength;
uniform float horizontalStrength;

vec2 sineWave(vec2 pt)
{
    float x = 0.0;
    float y = 0.0;

    if (effectType == EFFECT_TYPE_DREAMY)
    {
        float offsetX = sin(pt.y * uFrequency + uTime * uSpeed) * uWaveAmplitude;
        pt.x += offsetX; // * (pt.y - 1.0); // <- Uncomment to stop bottom part of the screen from moving
    }
    else if (effectType == EFFECT_TYPE_WAVY)
    {
        float offsetY = sin(pt.x * uFrequency + uTime * uSpeed) * uWaveAmplitude;
        pt.y += offsetY; // * (pt.y - 1.0); // <- Uncomment to stop bottom part of the screen from moving
    }
    else if (effectType == EFFECT_TYPE_HEAT_WAVE_HORIZONTAL)
    {
        x = sin(pt.x * uFrequency + uTime * uSpeed) * uWaveAmplitude;
    }
    else if (effectType == EFFECT_TYPE_HEAT_WAVE_VERTICAL)
    {
        y = sin(pt.y * uFrequency + uTime * uSpeed) * uWaveAmplitude;
    }
    else if (effectType == EFFECT_TYPE_FLAG)
    {
        y = sin(pt.y * uFrequency + 10.0 * pt.x + uTime * uSpeed) * uWaveAmplitude;
        x = sin(pt.x * uFrequency + 5.0 * pt.y + uTime * uSpeed) * uWaveAmplitude;
    }
    else if (effectType == EFFECT_TYPE_HEAT_WAVE_BOTH)
    {
        x = sin(pt.x * (uFrequency * horizontalStrength) + uTime * (uSpeed * horizontalStrength)) * (uWaveAmplitude * horizontalStrength);
        y = sin(pt.y * (uFrequency * verticalStrength) + uTime * (uSpeed * verticalStrength)) * (uWaveAmplitude * verticalStrength);
    }

    return vec2(pt.x + x, pt.y + y);
}

void main()
{
    vec2 uv = sineWave(openfl_TextureCoordv);
    gl_FragColor = texture2D(bitmap, uv);
}
)GLSL";

// GlitchShader (from Dave and Bambi)
constexpr const char* kGlitch = R"GLSL(#pragma header
//uniform float tx, ty; // x,y waves phase

//modified version of the wave shader to create weird garbled corruption like messes
uniform float uTime;

/**
 * How fast the waves move over time
 */
uniform float uSpeed;

/**
 * Number of waves over time
 */
uniform float uFrequency;

/**
 * How much the pixels are going to stretch over the waves
 */
uniform float uWaveAmplitude;

vec2 sineWave(vec2 pt)
{
    float x = 0.0;
    float y = 0.0;

    float offsetX = sin(pt.y * uFrequency + uTime * uSpeed) * (uWaveAmplitude / pt.x * pt.y);
    float offsetY = sin(pt.x * uFrequency - uTime * uSpeed) * (uWaveAmplitude / pt.y * pt.x);
    pt.x += offsetX; // * (pt.y - 1.0); // <- Uncomment to stop bottom part of the screen from moving
    pt.y += offsetY;

    return vec2(pt.x + x, pt.y + y);
}

void main()
{
    vec2 uv = sineWave(openfl_TextureCoordv);
    gl_FragColor = texture2D(bitmap, uv);
}
)GLSL";

// DistortBGShader
constexpr const char* kDistort = R"GLSL(#pragma header
//uniform float tx, ty; // x,y waves phase

//gives the character a glitchy, distorted outline
uniform float uTime;

/**
 * How fast the waves move over time
 */
uniform float uSpeed;

/**
 * Number of waves over time
 */
uniform float uFrequency;

/**
 * How much the pixels are going to stretch over the waves
 */
uniform float uWaveAmplitude;

vec2 sineWave(vec2 pt)
{
    float x = 0.0;
    float y = 0.0;

    float offsetX = sin(pt.x * uFrequency + uTime * uSpeed) * (uWaveAmplitude / pt.x * pt.y);
    float offsetY = sin(pt.y * uFrequency - uTime * uSpeed) * (uWaveAmplitude);
    pt.x += offsetX; // * (pt.y - 1.0); // <- Uncomment to stop bottom part of the screen from moving
    pt.y += offsetY;

    return vec2(pt.x + x, pt.y + y);
}

vec4 makeBlack(vec4 pt)
{
    // FIX: vec4(0, 0, 0, pt.w) -> vec4(0.0, 0.0, 0.0, pt.w) (integer args in vector constructor)
    return vec4(0.0, 0.0, 0.0, pt.w);
}

void main()
{
    vec2 uv = sineWave(openfl_TextureCoordv);
    gl_FragColor = makeBlack(texture2D(bitmap, uv)) + texture2D(bitmap,openfl_TextureCoordv);
}
)GLSL";

// RuntimeShaders.pulseEffect: off unless uEnabled (only the Rainbow Eyesore event turns it on)
constexpr const char* kPulse = R"GLSL(#pragma header

uniform float uAmplitude;
uniform float uTime;
uniform float uSpeed;
uniform float uFrequency;
uniform bool uEnabled;
uniform float uWaveAmplitude;

vec4 sineWave(vec4 pt)
{
    if (uWaveAmplitude > 0.0 && uEnabled)
    {
        // Calculate offsets
        float offsetX = sin(pt.y * uFrequency + uTime * uSpeed);
        float offsetY = sin(pt.x * (uFrequency * 2.0) - (uTime / 2.0) * uSpeed);
        float offsetZ = sin(pt.z * (uFrequency / 2.0) + (uTime / 3.0) * uSpeed);

        // Use (0.5 * sin + 0.5) to keep values between 0.0 and 1.0
        // This prevents negative colors (Black Screen)
        float weirdX = 0.5 * sin(pt.x / 2.0 * pt.y + (5.0 * offsetX) * pt.z) + 0.5;
        pt.x = mix(pt.x, weirdX, uWaveAmplitude * uWaveAmplitude);

        float weirdY = 0.5 * sin(pt.y / 3.0 * pt.z + (2.0 * offsetZ) - pt.x) + 0.5;
        pt.y = mix(pt.y, weirdY, uWaveAmplitude * uWaveAmplitude);

        float weirdZ = 0.5 * sin(pt.z / 6.0 * (pt.x * offsetY) - (50.0 * offsetZ) * (pt.z * offsetX)) + 0.5;
        pt.z = mix(pt.z, weirdZ, uWaveAmplitude * uWaveAmplitude);
    }

    return vec4(pt.x, pt.y, pt.z, pt.w);
}

void main()
{
    vec2 uv = openfl_TextureCoordv;
    gl_FragColor = sineWave(flixel_texture2D(bitmap, uv));
}
)GLSL";

// InvertShader (it inverts the alpha too: over the black stage, with premultiplied blending, the colours come out inverted)
constexpr const char* kInvert = R"GLSL(#pragma header

vec4 sineWave(vec4 pt)
{

return vec4(1.0 - pt.x, 1.0 - pt.y, 1.0 - pt.z, pt.w);
}

void main()
{
    vec2 uv = openfl_TextureCoordv;
    gl_FragColor = sineWave(texture2D(bitmap, uv));
    gl_FragColor.a = 1.0 - gl_FragColor.a;
}
)GLSL";

// GreyscaleShader
constexpr const char* kGreyscale = R"GLSL(#pragma header
void main() {
    vec4 color = texture2D(bitmap, openfl_TextureCoordv);
    float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    gl_FragColor = vec4(vec3(gray), color.a);
}
)GLSL";

// ThreeDShader (gl_FragColor is cleared first: JS Engine leaves it unset off the card)
constexpr const char* k3D = R"GLSL(    #pragma header
    uniform float xrot;
    uniform float yrot;
    uniform float zrot;
    uniform float dept;
    // FIX: float alph = 0 -> float alph = 0.0 (integer init for float)
    float alph = 0.0;

float plane( in vec3 norm, in vec3 po, in vec3 ro, in vec3 rd ) {
    float de = dot(norm, rd);
    de = sign(de)*max( abs(de), 0.001);
    return dot(norm, po-ro)/de;
}

vec2 raytraceTexturedQuad(in vec3 rayOrigin, in vec3 rayDirection, in vec3 quadCenter, in vec3 quadRotation, in vec2 quadDimensions) {
    //Rotations ------------------
    float a = sin(quadRotation.x); float b = cos(quadRotation.x);
    float c = sin(quadRotation.y); float d = cos(quadRotation.y);
    float e = sin(quadRotation.z); float f = cos(quadRotation.z);
    float ac = a*c;   float bc = b*c;

    mat3 RotationMatrix  =
            mat3(      d*f,      d*e,  -c,
                 ac*f-b*e, ac*e+b*f, a*d,
                 bc*f+a*e, bc*e-a*f, b*d );
    //--------------------------------------

    vec3 right = RotationMatrix * vec3(quadDimensions.x, 0.0, 0.0);
    // FIX: vec3(0, quadDimensions.y, 0) -> vec3(0.0, quadDimensions.y, 0.0) (integer args in vector constructor)
    vec3 up = RotationMatrix * vec3(0.0, quadDimensions.y, 0.0);
    vec3 normal = cross(right, up);
    normal /= length(normal);

    //Find the plane hit point in space
    vec3 pos = (rayDirection * plane(normal, quadCenter, rayOrigin, rayDirection)) - quadCenter;

    //Find the texture UV by projecting the hit point along the plane dirs
    return vec2(dot(pos, right) / dot(right, right),
                dot(pos, up)    / dot(up,    up)) + 0.5;
}

void main() {
    gl_FragColor = vec4(0.0);  // left unset off the card in JS Engine
    vec4 texColor = texture2D(bitmap, openfl_TextureCoordv);
    //Screen UV goes from 0 - 1 along each axis
    vec2 screenUV = openfl_TextureCoordv;
    vec2 p = (2.0 * screenUV) - 1.0;
    // FIX: 1280/720 -> 1280.0/720.0 (integer division would give wrong result)
    float screenAspect = 1280.0/720.0;
    p.x *= screenAspect;

    //Normalized Ray Dir
    vec3 dir = vec3(p.x, p.y, 1.0);
    dir /= length(dir);

    //Define the plane
    vec3 planePosition = vec3(0.0, 0.0, dept);
    vec3 planeRotation = vec3(xrot, yrot, zrot);//this the shit you needa change
    vec2 planeDimension = vec2(-screenAspect, 1.0);

    // FIX: vec3(0) -> vec3(0.0, 0.0, 0.0) (integer arg in vector constructor)
    vec2 uv = raytraceTexturedQuad(vec3(0.0, 0.0, 0.0), dir, planePosition, planeRotation, planeDimension);

    //If we hit the rectangle, sample the texture
    if (abs(uv.x - 0.5) < 0.5 && abs(uv.y - 0.5) < 0.5) {

        vec3 tex = flixel_texture2D(bitmap, uv).xyz;
        float bitch = 1.0;
        if (tex.z == 0.0){
            bitch = 0.0;
        }

      gl_FragColor = vec4(flixel_texture2D(bitmap, uv).xyz, bitch);
    }
}
)GLSL";

// BloomShader
constexpr const char* kBloom = R"GLSL(    #pragma header

    uniform float intensity;
    uniform float blurSize;

void main()
{
   // FIX: vec4(0) -> vec4(0.0) (integer arg in vector constructor)
   vec4 sum = vec4(0.0);
   vec2 texcoord = openfl_TextureCoordv;
   // FIX: removed unused int j; int i; declarations

   //thank you! http://www.gamerendering.com/2008/10/11/gaussian-blur-filter-shader/ for the
   //blur tutorial
   // blur in y (vertical)
   // take nine samples, with the distance blurSize between them
   sum += flixel_texture2D(bitmap, vec2(texcoord.x - 4.0*blurSize, texcoord.y)) * 0.05;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x - 3.0*blurSize, texcoord.y)) * 0.09;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x - 2.0*blurSize, texcoord.y)) * 0.12;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x - blurSize, texcoord.y)) * 0.15;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y)) * 0.16;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x + blurSize, texcoord.y)) * 0.15;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x + 2.0*blurSize, texcoord.y)) * 0.12;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x + 3.0*blurSize, texcoord.y)) * 0.09;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x + 4.0*blurSize, texcoord.y)) * 0.05;

    // blur in y (vertical)
   // take nine samples, with the distance blurSize between them
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y - 4.0*blurSize)) * 0.05;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y - 3.0*blurSize)) * 0.09;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y - 2.0*blurSize)) * 0.12;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y - blurSize)) * 0.15;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y)) * 0.16;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y + blurSize)) * 0.15;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y + 2.0*blurSize)) * 0.12;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y + 3.0*blurSize)) * 0.09;
   sum += flixel_texture2D(bitmap, vec2(texcoord.x, texcoord.y + 4.0*blurSize)) * 0.05;

   //increase blur with intensity!
  gl_FragColor = sum*intensity + flixel_texture2D(bitmap, texcoord);
}
)GLSL";

// BlockedGlitchShader (shadertoy.com/view/MlVSD3): 64 bands slide sideways each frame, red and blue
// further than green
constexpr const char* kBlocked = R"GLSL(#pragma header

// ---- gllock required fields -----------------------------------------------------------------------------------------
#define RATE 0.75
uniform float time;
uniform float end;
uniform sampler2D imageData;
uniform vec2 screenSize;
// ---------------------------------------------------------------------------------------------------------------------

float rand(vec2 co){
  return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453) * 2.0 - 1.0;
}

float offset(float blocks, vec2 uv) {
  float shaderTime = time*RATE;
  return rand(vec2(shaderTime, floor(uv.y * blocks)));
}

void main(void) {
  vec2 uv = openfl_TextureCoordv;
  gl_FragColor = flixel_texture2D(bitmap, uv);
  gl_FragColor.r = flixel_texture2D(bitmap, uv + vec2(offset(64.0, uv) * 0.03, 0.0)).r;
  gl_FragColor.g = flixel_texture2D(bitmap, uv + vec2(offset(64.0, uv) * 0.03 * 0.16666666, 0.0)).g;
  gl_FragColor.b = flixel_texture2D(bitmap, uv + vec2(offset(64.0, uv) * 0.03, 0.0)).b;
}
)GLSL";

struct Builtin {
    const char* name;
    const char* frag;
};
const Builtin kBuiltins[] = {
    {"__fx_chromatic", kChromatic}, {"__fx_scanline", kScanline}, {"__fx_grain", kGrain},
    {"__fx_tiltshift", kTiltshift}, {"__fx_vcr", kVcr},          {"__fx_wiggle", kWiggle},
    {"__fx_glitch", kGlitch},       {"__fx_distort", kDistort},  {"__fx_pulse", kPulse},
    {"__fx_invert", kInvert},       {"__fx_greyscale", kGreyscale}, {"__fx_3d", k3D},
    {"__fx_bloom", kBloom},         {"__fx_blocked", kBlocked},
};

}  // namespace

std::string_view source(std::string_view name) {
    for (const Builtin& b : kBuiltins)
        if (name == b.name) return b.frag;
    return {};
}

bool install(scene::Scene& sc, const std::string& name) {
    if (sc.shaderSources.count(name)) return true;
    const std::string_view src = source(name);
    if (src.empty()) return false;
    sc.shaderSources[name].frag = std::string(src);
    return true;
}

scene::ShaderPtr make(scene::Scene& sc, const std::string& name, bool timed, const std::string& timeName, float start) {
    if (!install(sc, name)) return nullptr;
    auto inst = std::make_shared<scene::ShaderInstance>();
    inst->name = name;
    inst->timeName = timeName;
    inst->floats[timeName] = {start};
    if (timed) inst->timeRate = 1.0f;
    return inst;
}

int wiggleType(std::string_view name) {
    std::string n;
    for (char c : name)
        if (c != '_' && c != '-' && !std::isspace((unsigned char)c)) n += char(std::tolower((unsigned char)c));
    if (n == "wavy") return 1;
    if (n == "horizontal" || n == "heatwavehorizontal") return 2;
    if (n == "vertical" || n == "heatwavevertical") return 3;
    if (n == "flag") return 4;
    if (n == "both" || n == "heatwaveboth") return 5;
    return 0;  // dreamy
}

}  // namespace gh::fx
