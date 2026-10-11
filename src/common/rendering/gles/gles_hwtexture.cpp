/*
** gles_hwtexture.cpp
**
** GL texture abstraction
**
**---------------------------------------------------------------------------
**
** Copyright 2019 Christoph Oelckers
** Copyright 2019-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "nx_perf.h"
#include "gles_asynctex.h"
#include "c_cvars.h"
#include "printf.h"
EXTERN_CVAR(Int, nx_perflog)
#include "gles_system.h"

#include "c_cvars.h"
#include "hw_material.h"

#include "hw_cvars.h"
#include "gles_renderer.h"
#include "gles_samplers.h"
#include "gles_renderstate.h"
#include "gles_hwtexture.h"

namespace OpenGLESRenderer
{


TexFilter_s TexFilter[] = {
	{GL_NEAREST,					GL_NEAREST,		false},
	{GL_NEAREST_MIPMAP_NEAREST,		GL_NEAREST,		true},
	{GL_LINEAR,						GL_LINEAR,		false},
	{GL_LINEAR_MIPMAP_NEAREST,		GL_LINEAR,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_LINEAR,		true},
	{GL_NEAREST_MIPMAP_LINEAR,		GL_NEAREST,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_NEAREST,		true},
};

//===========================================================================
//
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];

//===========================================================================
//
// Switch port: mipmaps are built on the CPU and uploaded level by level.
//
// glGenerateMipmap goes through Mesa's blitter on the Switch: one draw per
// mip level with a full save/restore of the GL state around it, and the next
// real draw has to revalidate everything. That made every new texture cost
// milliseconds (even tiny ones), which showed up as hitches whenever new
// sprites appeared (fire, explosions, monsters). A 2x2 box filter on the CPU
// costs a few microseconds for a sprite.
//
//===========================================================================

CVAR(Bool, gles_cpu_mipmaps, true, CVAR_GLOBALCONFIG)

static int nxSkipCpuMips;

void NX_PushSkipCpuMipmaps() { nxSkipCpuMips++; }
void NX_PopSkipCpuMipmaps() { if (nxSkipCpuMips > 0) nxSkipCpuMips--; }

// Sprites are small powers of two: a box filter on the CPU is cheaper than
// Mesa's blitter. Large and non-power-of-two images (the WadFusion title
// picture, skies, hi-res walls) stay on glGenerateMipmap. Running the CPU
// path on those while the next map was precached left the title picture
// on screen and never came back.
static bool NXCpuMipmapSize(int w, int h)
{
	if (w < 1 || h < 1 || w > 512 || h > 512) return false;
	return (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
}

static void NXDownsample(const unsigned char *src, int sw, int sh, unsigned char *dst, int dw, int dh, int bpp)
{
	// 2x2 box filter (1x2 or 2x1 once one side is down to 1 pixel)
	const int sx = sw > 1 ? 2 : 1, sy = sh > 1 ? 2 : 1;
	const size_t srow = size_t(sw) * bpp;
	for (int y = 0; y < dh; y++)
	{
		const unsigned char *r0 = src + size_t(y * sy) * srow;
		const unsigned char *r1 = sy == 2 ? r0 + srow : r0;
		unsigned char *d = dst + size_t(y) * dw * bpp;
		for (int x = 0; x < dw; x++)
		{
			const unsigned char *a = r0 + size_t(x * sx) * bpp, *b = a + (sx - 1) * bpp;
			const unsigned char *c = r1 + size_t(x * sx) * bpp, *e = c + (sx - 1) * bpp;
			for (int i = 0; i < bpp; i++) d[i] = (unsigned char)((a[i] + b[i] + c[i] + e[i] + 2) >> 2);
			d += bpp;
		}
	}
}

static void NXUploadMipmaps(const unsigned char *level0, int w, int h, int bpp, int texformat, int sourcetype)
{
	TArray<unsigned char> bufA, bufB;
	const unsigned char *src = level0;
	int level = 0;
	while (w > 1 || h > 1)
	{
		int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
		TArray<unsigned char> &dst = (level & 1) ? bufB : bufA;
		dst.Resize(unsigned(nw) * nh * bpp);
		NXDownsample(src, w, h, dst.Data(), nw, nh, bpp);
		level++;
		glTexImage2D(GL_TEXTURE_2D, level, texformat, nw, nh, 0, sourcetype, GL_UNSIGNED_BYTE, dst.Data());
		src = dst.Data();
		w = nw;
		h = nh;
	}
}

//===========================================================================
//
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================

unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	int rh,rw;
	int texformat = GL_RGBA;// TexFormat[gl_texture_format];
	bool deletebuffer=false;

	// When running in SW mode buffer will be null, so set it to the texBuffer already created
	// There could be other use cases I do not know about which means this is a bad idea..
	if (buffer == nullptr)
		buffer = texBuffer;

	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);


	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (!buffer)
	{
		// The texture must at least be initialized if no data is present.
		mipmapped = false;
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		//texheight=-h;
	}
	else
	{
		if (rw < w || rh < h)
		{
			// The texture is larger than what the hardware can handle so scale it down.
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				Resize(w, h, rw, rh, buffer, scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}


	}
	// store the physical size.

	int sourcetype;

	if (gles.glesMode == GLES_MODE_GLES)
	{
		if (glTextureBytes == 1)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_ALPHA;
			texformat = GL_ALPHA;
		}
		else
		{
			sourcetype = GL_BGRA; // These two must be the same
			texformat = GL_BGRA;
		}
	}
	else
	{
		if (glTextureBytes == 1) //Use Red channel instread becuase Alpha does not work in OpenGL, swizzle later
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_RED;
			texformat = GL_RED;
		}
		else
		{
			sourcetype = GL_BGRA;
			texformat = GL_RGBA;
		}
	}

	glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);

	if (gles.glesMode != GLES_MODE_GLES)
	{
		// The shader is using the alpha channel instead of red, this work on GLES but not on GL
		// So the texture uses GL_RED and this swizzels the red channel into the alpha channel
		if (glTextureBytes == 1)
		{
			GLint swizzleMask[] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);
		}
	}

	if (mipmap && TexFilter[gl_texture_filter].mipmapping)
	{
		// One sprite at a time, during play, is cheap on the CPU. A precache or a
		// background batch of a whole mod is not: that runs while the WadFusion
		// title is already on screen and never comes back. Those batches use
		// the same path as nx3.7.
		if (nxSkipCpuMips == 0 && gles_cpu_mipmaps && buffer && (glTextureBytes == 1 || glTextureBytes == 4) && NXCpuMipmapSize(rw, rh))
			NXUploadMipmaps(buffer, rw, rh, glTextureBytes, texformat, sourcetype);
		else
			glGenerateMipmap(GL_TEXTURE_2D);
		mipmapped = true;
	}

	if (deletebuffer && buffer) free(buffer);

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
	return glTexID;
}


void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;

	if (texBuffer)
		delete[] texBuffer;

	texBuffer = new uint8_t[(w * h) * texelsize];
	return;
}

uint8_t* FHardwareTexture::MapBuffer()
{
	return texBuffer;
}

//===========================================================================
//
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture()
{
	if (glTexID != 0) glDeleteTextures(1, &glTexID);

	if (texBuffer)
		delete[] texBuffer;
}


//===========================================================================
//
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, bool needmipmap)
{
	if (glTexID != 0)
	{
		if (lastbound[texunit] == glTexID) return glTexID;
		lastbound[texunit] = glTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, glTexID);
		// Check if we need mipmaps on a texture that was creted without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return glTexID;
	}
	return 0;
}

void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}

//===========================================================================
//
//	Creates a depth buffer for this texture
//
//===========================================================================

int FHardwareTexture::GetDepthBuffer(int width, int height)
{
	if (glDepthID == 0)
	{
		glGenRenderbuffers(1, &glDepthID);
		glBindRenderbuffer(GL_RENDERBUFFER, glDepthID);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
			GetTexDimension(width), GetTexDimension(height));
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	return glDepthID;
}


//===========================================================================
//
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer(int width, int height)
{
	width = GetTexDimension(width);
	height = GetTexDimension(height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
}


//===========================================================================
//
//	Binds a texture to the renderer
//
//===========================================================================

bool FHardwareTexture::BindOrCreate(FTexture *tex, int texunit, int clampmode, int translation, int flags)
{
	bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	// Bind it to the system.
	if (!Bind(texunit, needmipmap))
	{
		NXPERF_SCOPE(tex_ms, &nxperf.textures);
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;

		// Create this texture

		FTextureBuffer texbuffer;

		if (!tex->isHardwareCanvas())
		{
			int async = (flags & CTF_Indexed) ? -1 : AsyncTex_Request(tex, translation, flags | CTF_ProcessData, texbuffer);
			if (async == 0)
			{
				// still being decoded in the background: draw nothing for now
				AsyncTex_BindPlaceholder(texunit);
				GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
				gl_RenderState.ClearLastMaterial();
				return true;
			}
			if (async < 0) texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			w = texbuffer.mWidth;
			h = texbuffer.mHeight;
		}
		else
		{
			w = tex->GetWidth();
			h = tex->GetHeight();
		}
		nxperf.texpixels += (int64_t)w * h;
		if (nx_perflog >= 3)
		{
			FString src = tex->GetImage() ? FString("img") : FString("noimg");
			Printf("NXTEX %p tr=%d fl=%x %dx%d decode=%.2fms\n", (void*)tex, translation, flags, w, h, NXPerf_NowMS() - nxperf_scope_.start);
		}
		bool created = CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreate");
		AsyncTex_Uploaded();
		if (!created)
		{
			// could not create texture
			return false;
		}
	}
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
