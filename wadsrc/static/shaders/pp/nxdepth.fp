/*
** nxdepth.fp
** Half-resolution effects layer (RDoom, Switch): the scene depth of the
** (dynamic-resolution) view copied to a float texture, read like SSAO's
** lineardepth.fp does; a second pass turns it into the layer's depth buffer.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D DepthTexture;

void main()
{
	vec2 uv = Offset + TexCoord * Scale;
	ivec2 ipos = ivec2(max(uv * vec2(textureSize(DepthTexture, 0)), vec2(0.0)));
	FragColor = vec4(texelFetch(DepthTexture, ipos, 0).x, 0.0, 0.0, 1.0);
}
