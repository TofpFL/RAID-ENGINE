// Psych Engine / Flixel shaders are OpenFL fragment shaders in GLSL ES 1.0
// style ("#pragma header", bitmap, openfl_TextureCoordv, flixel_texture2D,
// gl_FragColor). translateShader() turns one into a GLSL 330 fragment shader
// that runs with raylib's default vertex shader.
//
// Uniforms the renderer sets on top of the shader's own:
//   openfl_TextureSize  size of the texture being drawn
//   gh_flipY            1 when drawing a render texture (stored upside down)
//   gh_premul           1 when the texture holds straight (not premultiplied) alpha
// The output is premultiplied, like OpenFL's, so it is drawn with
// premultiplied-alpha blending.
#pragma once

#include <string>
#include <string_view>

namespace gh {

std::string translateShader(std::string_view openflFragment);

}  // namespace gh
