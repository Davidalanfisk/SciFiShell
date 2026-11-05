# Music visualizer plan

Goal: a hardware-accelerated, open-source music visualizer for the media player (Milkdrop-style presets).

## Candidate: projectM

[projectM](https://github.com/projectM-visualizer/projectm) is an open-source reimplementation of Milkdrop. It is
the best fit: active, LGPL, thousands of presets, and it takes PCM samples as input.

## The catch

projectM renders with **OpenGL** (it needs a GL 3.3 core context). SciFiShell renders with **Direct3D 12** through
Diligent Engine, so projectM cannot draw straight into the shell's swapchain.

## Plan

1. **Audio feed.** Tap the PCM that the media player already decodes for the live spectrum and push it to projectM
   with `projectm_pcm_add_float`.
2. **Offscreen GL context.** Create a hidden SDL2 OpenGL window (or a WGL pbuffer) on a dedicated visualizer thread.
   projectM renders into an FBO of a fixed size (for example 1024x576).
3. **Hand the frame to D3D12.** Two options, in order of preference:
   - *Fast:* the `WGL_NV_DX_interop2` extension shares the D3D resource with the GL FBO with no copy.
   - *Simple:* `glReadPixels` into a CPU buffer and upload it as the same kind of `Bitmap` the video player already
     uses for frames. At 1024x576 and 30 fps this is cheap enough to start with.
4. **UI.** A `VISUALIZER` button in the now-playing bar and the music views. It shows the output in the media window
   (and fullscreen), with next/previous preset keys. Presets live in a user folder so people can add their own.
5. **Packaging.** Build projectM through `FetchContent` behind a CMake option (`SCIFI_VISUALIZER`) so the default
   build stays small. Ship a handful of permissively licensed presets with the installer.

## Why not now

The interop and thread plumbing is the real work and needs testing across GPUs. The existing spectrum bar stays as the
built-in visualizer until this lands.
