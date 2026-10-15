# Mac OpenGL bring-up log

15 October 2026. What a native Xbox decompilation on Apple Silicon actually
does, which pieces belong in this toolkit, and what the POSIX backend can do
after they landed.

This is a toolkit log. Title addresses and per-game hooks stay in that title's
project. The rules that follow from this session are
[GOLDEN_RULES.md](../GOLDEN_RULES.md).

## Two ways to get a native ARM game

A decompilation recovers the game as C, compiles that C with clang, and
replaces the Xbox kit with a new implementation of `D3DDevice_*`. The kit's
GPU command writers are not in the program. Draws are calls into the port.

This toolkit lifts every x86 instruction in the XBE, including the kit that
was linked into the disc. Those functions still write NV2A commands. The
process has no NV2A, so the writes do not become pixels. The OpenGL device in
`d3d8_gl.c` runs only when something calls it.

Both end at the same GPU API on a Mac. OpenGL 4.1, which Apple implements on
Metal. Neither path is a hand-written Metal renderer. The speed difference is
the CPU: recovered C or lifted C, compiled to ARM, against one translated
Pentium.

## What was taken from the Halo Mac port

The reference is the `macos-64bit` branch of
[ChupathingyCE](https://github.com/ChupathingyCE/chupathingyce), which builds
the Linux port as 64-bit arm64. The useful code is under `port/`. The game
sources are that title's and were not copied.

Taken:

- The vertex-microcode translator. The encoding is the GPU's. It is now
  `src/d3d/nv2a_glsl.c`. `D3DDevice_CreateVertexShader` stores the words.
  `SetVertexShaderConstant` stores the 192 constant vectors. The first draw
  that binds the handle compiles the GLSL and caches the program. An FVF
  (shader handle with the high bit clear) still uses the fixed
  position-color-UV program.
- The Apple driver constraints, from that port's Mac notes. The context is
  GL 4.1. The generated shader flips Y and remaps depth because there is no
  `glClipControl`. Stream uploads allocate a fresh buffer, because
  `glBufferSubData` on a buffer the GPU is still reading stalls. Swap interval
  stays 0 on Apple, because SDL's wait needs the main-thread run loop and the
  title draws on another thread.

Left where they were:

- That port's device implementation. It reads that game's render-state globals
  and assumes a 480-line screen.
- That port's pixel-combiner translator. Its key is a private table whose
  first entry is 0. Our `D3DRS` values start at 200 for the same state.
  Dropping the file in would assemble the wrong shader. Combiners on POSIX are
  still the fixed "color times texture" fragment shader. The opcode logic is
  the right source when this is ported, keyed on `d3d8_xbox.h`.
- That port's 4 GB window at a high fixed address. `xbox_memory_layout.c`
  already maps guest memory with one offset, because macOS reserves the low
  4 GB. A second map would fight it.

The Windows backend is unchanged. It still translates combiners and vertex
microcode to HLSL. See [nv2a-shaders.md](nv2a-shaders.md).

## What the POSIX backend does now

`src/d3d/CMakeLists.txt` builds `d3d8_gl.c` and `nv2a_glsl.c` into
`xbox_d3d8` on every non-Windows host.

`d3d8_HostOpenWindow`, `d3d8_HostCreateDevice`, `d3d8_HostMainLoop`, and
`d3d8_HostPump` are the window API for a title whose own `CreateDevice` is
code inside the XBE. On Apple, `CreateDevice` from a thread other than the
main thread returns without creating the window. Cocoa drops a window whose
thread stops answering. The main thread opens the window and pumps it.

`RECOMP_NO_EXIT`, if set in the environment, makes `HalReturnToFirmware`
return to the title instead of ending the process. A title that cannot find
the dashboard reboots through that call. Unset, the process still exits.
`NtCreateFile` logs the guest path and the errno when the host file is missing.

A weak `d3d8_HostPump` runs on the way out of a kernel thunk when the OpenGL
backend is linked and the main loop is not already pumping. A Windows link
does not have the symbol. The pump is not a substitute for the title calling
`Present`.

## What a frame still needs

The translator is unused until the title's D3D calls are the backend's calls.
A prefix at the top of the lifted `CreateDevice` opens a window and then lets
the original body program a GPU that is not there. The next relift deletes
the prefix. The replacement goes in the title's `recomp_lookup_manual`, and
the lifted body does not run.

Until those replacements exist, the order that produces a picture is:

1. The host thread pumps and resizes. Swapping happens in `Present`. A host
   loop that clears and swaps every iteration erases the frame.
2. Replace the kit entry points the title actually calls: device creation,
   vertex and index buffers, textures, stream source, vertex shader, shader
   constants, texture bind, render state, draw, clear, present. Log the
   addresses first. Do not invent the list.
3. Feed `v0` onward from the vertex declaration. The fixed FVF layout is the
   wrong inputs for a real program.
4. Unswizzle textures on upload.
5. Port combiner opcodes onto our render-state numbers once a mesh is visible.
   A combiner bug cannot be judged while draws never arrive.

`libxbox_d3d8` builds with the translator on this Mac. That build does not
mean a title has reached `DrawPrimitive`.
