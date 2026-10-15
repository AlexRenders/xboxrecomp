# Golden Rules

These rules keep xboxrecomp a toolkit for many titles. A rule that only makes
one game boot belongs in that title's project, not here.

The Windows D3D11 path and the POSIX OpenGL path are both first-class. A fix
for one host does not rewrite the other.

## 1. The toolkit does not learn a title's addresses

Layout comes from the XBE: image base, section sizes, kernel imports, entry
point. A virtual address, a present-parameter offset, a dashboard path, or a
"keep going past this reboot" switch is that title's `recomp_manual.c`.

`RECOMP_NO_EXIT` is the allowed shape when a bring-up switch has to live in
the runtime. It is off unless the operator sets it. The default path of every
other title stays the same.

## 2. Replace the guest function

A call inserted at the top of a lifted body is deleted by the next relift, and
the original body still runs after it. To change what every caller does,
replace the function. `recomp_lookup_manual` and `--exclude-manual` are the
two mechanisms. See [Replace, Don't Wrap](pipeline/05-runtime.md#replace-dont-wrap).

The replacement reads the stdcall arguments off the guest stack, calls the
runtime, sets `g_eax`, and returns. It cleans the stack the way the original
`ret N` did.

## 3. Answer D3D. Do not emulate the pushbuffer

The XDK linked into a retail XBE writes NV2A commands. This process has no
NV2A. Those writes do not become pixels.

The picture is the D3D8 backend. Windows speaks D3D11. POSIX speaks OpenGL.
Both answer `CreateDevice`, resource locks, shader and texture binds, draw,
clear, and `Present`. Re-implementing the command stream is an emulator, and
it is a different project.

## 4. Take the hardware, leave the game

Vertex-shader microcode is the GPU's encoding. A translator for it belongs in
this repo and serves every title. `src/d3d/nv2a_glsl.c` is that translator for
OpenGL. The HLSL path stays in the Windows backend.

A decompiled title's device implementation does not belong here. It reads that
game's globals and that game's screen size. Copying it in makes every other
title wrong.

Pixel combiners are hardware too, but the key has to be the XDK render-state
numbers in `d3d8_xbox.h`. A port that packs those states into its own 0-based
table will read the wrong slots if it is dropped onto this backend. Port the
opcode logic. Keep our state numbers.

## 5. One memory map

macOS reserves the low 4 GB, so guest address 0 cannot be host address 0.
`xbox_memory_layout.c` already applies one offset. A second window, copied
from another port, fights that map. Guest pointers stay guest pointers. The
offset is applied at the boundary.

## 6. Apple's OpenGL is a translator

The newest context Apple ships is OpenGL 4.1, and it runs on Metal.

- There is no `glClipControl`. The vertex shader flips Y and remaps depth.
- `glBufferSubData` on a buffer the GPU is still reading stalls the frame.
  Allocate a fresh buffer for the upload.
- SDL's swap interval waits on the main-thread run loop. A title that draws
  on another thread leaves the interval at 0. The thread that owns the window
  is the one that presents.
- The fixed-function fragment shader may stay on `#version 330`. That profile
  is valid on a 4.1 context. Generated vertex shaders on Apple use 410.

None of these change the Windows backend.

## 7. Present is the title's Present

The host thread pumps events and applies a resize. It does not clear and swap
on its own. A loop that paints a color every iteration erases the frame the
title just drew.

## 8. The declaration picks the attributes

A shader handle with the high bit set is a vertex program. Its inputs are the
declaration passed to `CreateVertexShader`, in order, starting at `v0`. The
position-color-UV layout is the fallback for an FVF (high bit clear). It is
not the layout of every program.

## 9. Unswizzle on upload

Xbox textures are swizzled. The upload that follows an unlock writes the
linear image. A shader that samples a still-swizzled texture is not a bad
shader.

## 10. Log the call, then replace it

The first session on a new title records which kernel ordinals and which D3D
entry points actually run. Replacements are written against that list. Guessing
a full D3D surface up front hides the call the title is stuck on.

File misses log the guest path and the errno. Indirect calls that miss the
dispatch table log the address. Both stay rate-limited when they fire in a loop.

## 11. No game bytes in this repo

No disc image, no XBE, no extracted asset, no save. Docs may name a title and
an XDK version. They do not carry its code or its data.

## 12. A host fix ships with the backend it belongs to

POSIX graphics changes land in `src/d3d/d3d8_gl.c` and `src/d3d/nv2a_glsl.c`,
and `src/d3d/CMakeLists.txt` lists every file the POSIX library compiles. A
new `.c` that is not in that list is not in the build. The HLSL translator is
not compiled into the POSIX library.
