# RDoom NX — engine

This is the engine of [RDoom NX](https://github.com/aeon1911/RDoom-NX): **UZDoom
5.0.3** with the patches that port it to the Nintendo Switch (Atmosphère
homebrew `.nro`), rendered with **Vulkan** through Mesa NVK. Branch
`rdoom-nx`; the same series is exported as `patches/` in the main repository.

Made by **Renan (Æon)**. Build, install, tested mods and donations: see the
[main repository](https://github.com/aeon1911/RDoom-NX).

What the port adds on top of UZDoom 5.0.3:

- Vulkan on the Switch (loaderless Mesa NVK, NAK thread-pointer bridge,
  workarounds for the Tegra X1), Vulkan only: the OpenGL/GLES backend is not
  built (`HAVE_OPENGL=OFF`, `HAVE_GLES2=OFF`).
- Per-frame performance work for handheld play at stock clocks: GPU-timed
  dynamic resolution, a half-resolution layer for smoke and fire, background
  sprite decoding, pipelines and textures prepared at level load, a pooled
  allocator for game objects, SD writes off the game thread.
- Switch platform layer (`src/common/platform/posix/nx/`): SD layout, logs,
  crash report, loading screen and the entry point of the RDoom launcher.

Stock clocks only: no overclocking code.

Built by `scripts/build.sh` in the main repository (Docker, devkitA64, libnx,
Mesa 26.2.4 Horizon SDK).

License: GPL-3.0, like UZDoom (see `LICENSE` and `docs/licenses/`).
