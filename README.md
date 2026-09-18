# Manifold

A game engine for spaces that are locally Euclidean and globally
whatever you wire them to be.

**Portals are not a feature bolted onto Manifold. They are the reason
it exists**, and the engine is built from the bottom up so that they
can be done the right way — which turns out to require decisions in the
projection maths, the clip space, the depth format and the render
hardware interface that no engine makes unless it means it.

C++20 core, Python scripting, Vulkan **and** OpenGL, Windows and Linux.
MIT licensed.

---

## Why it exists

The correct way to draw a portal has been known since 2007: mark the
aperture in the stencil buffer, redraw the world from a warped camera
inside that mask, and clip that inner view with a near plane lying
*exactly in the destination aperture* so nothing in front of it can be
drawn. It is exact. There is no seam, no resolution to choose, no
texture to filter, and the anti-aliasing matches the rest of the frame
because it *is* the rest of the frame.

It needs two things from an engine:

1. **Eight bits of stencil**, on the main depth buffer.
2. **A camera that is a matrix**, so the near plane can be oblique.

Mainstream engines give you the first and not the second. A camera is a
field of view and two clip planes, and the matrix is built behind your
back. Everything that makes portals hard elsewhere — the wall behind
the far portal creeping into shot, the wedge of view traded away to
hide it, the resolution ladders and clip biases that manage the
compromise — is downstream of not being allowed to touch sixteen
floats.

So in Manifold, `Camera3D` holds a `Projection`. `perspective()` and
`orthographic()` are constructors for one, not the definition of a
camera. And `Projection::with_oblique_near(plane)` is a public method
that any code may call.

## What is different, concretely

**One clip space for both backends.** Depth in `[0, 1]`, reversed, +Y
up. Vulkan gets a negative-height viewport; OpenGL gets
`glClipControl`. One projection matrix drives both, no shader contains
a per-backend branch, and `tests/test_backend_parity` renders the same
scene through both and requires **zero** differing pixels.

**Reverse-Z, and it is measured.** Two surfaces a centimetre apart at a
kilometre are separated by **138 float ULPs** under reverse-Z and by
**0** — the same float — under the standard mapping. The depth-stencil
format is `D32_SFLOAT_S8_UINT`, the only one that is both float (for
reverse-Z) and stencil-bearing (for portals); an adapter without it is
rejected at start-up rather than rendered to incorrectly.

**The oblique near plane, re-derived.** Lengyel's published form
assumes OpenGL's `[-1, 1]` depth. Manifold derives it again from the
clip condition itself, which is shorter and more general: it works for
off-axis frusta, for an already-oblique projection (a portal seen
through a portal) and for orthographic ones (a shadow cascade clipped
to a portal). `tests/test_math` sweeps 408 projection/plane
combinations and checks that a point on the plane lands exactly on the
near plane, that the right half-space survives, and that the x/y image
is bit-identical to the unmodified projection.

**Portals carry a size ratio.** Both ends of a pair need not be the
same size. The warp between a two-metre ring and a six-metre one is a
similarity transform with a factor of three in it, so walking through
makes you three times the size and the view through is magnified to
match. An unequal pair is a size machine, not a funnel.

**One shader source.** GLSL 4.50 with Vulkan semantics, compiled to
SPIR-V with `glslangValidator` for Vulkan and cross-compiled from that
same SPIR-V to GLSL 460 with `spirv-cross` for OpenGL. The two backends
cannot drift, because one is built from the other's output.

## Building

Needs a C++20 compiler, CMake 3.20, Python 3, SDL2, and — for the
shader build — `glslang-tools` and `spirv-cross`. Vulkan headers are
optional; the Vulkan backend is loaded with `dlopen` at run time, so a
machine with no Vulkan driver runs OpenGL rather than failing to start.

```sh
sudo apt install build-essential cmake ninja-build python3 \
                 libsdl2-dev glslang-tools spirv-cross libvulkan-dev \
                 python3-dev
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

```sh
build/bin/manifold --demo portals              # Vulkan by default
build/bin/manifold --demo portals --backend gl
build/bin/manifold --help
```

Right mouse captures the cursor, escape releases it. WASD moves, Q/E or
space/ctrl go up and down, shift hurries.

## Layout

```
src/core/          types, math, reflection, Variant
src/core/math/     vectors, quaternions, transforms, Projection
src/platform/      window, input, clock (SDL2)
src/rhi/           the render hardware interface
src/rhi/gl/        OpenGL 4.5 backend
src/rhi/vk/        Vulkan 1.3 backend
src/render/        meshes, materials, textures, the renderer
src/render/shaders/  one source per program, both backends
src/scene/         node tree, cameras, lights, Portal3D
tools/             the three code generators
tests/             maths, backend parity, the portal stencil sequence
docs/conventions.md  the rules, stated once
```

Three things are generated at build time from manifests checked into
the tree, never committed: the OpenGL loader
(`tools/glfns.txt`), the Vulkan loader (`tools/vkfns.txt`) and the
shaders. A name that is not in the system header is a build failure
rather than a null pointer at run time, and each manifest doubles as a
statement of exactly how much of that API the engine depends on.

## Status

Early, and honest about it.

Working: both backends at verified parity, the reverse-Z oblique
projection, the scene tree with reflection, PBR forward shading, a
procedural sky, filmic tonemapping, MSAA, and **recursive
stencil-clipped portals with per-pair size ratios**.

Not yet: shadows (the cascade plumbing is in, the pass is not),
physics, portal traversal for bodies, the plugin ABI, the Python
bindings, and the voxel terrain plugin.

See `docs/conventions.md` before touching the renderer.
