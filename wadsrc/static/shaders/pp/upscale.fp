/*
** upscale.fp
** Dynamic resolution (RDoom, Switch): the 3D view drawn into a smaller part
** of the scene texture, stretched over the full view.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;

void main()
{
	FragColor = texture(SceneTexture, Offset + TexCoord * Scale);
}
