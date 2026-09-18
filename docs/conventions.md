# Manifold conventions

Stated once, enforced by `tests/test_backend_parity`, which renders the
same scene through both backends and requires the images to match.

## Space

| | |
|---|---|
| Handedness | Right-handed. +X right, +Y up, −Z forward. A camera looks down its own −Z. |
| Matrices | Column-major storage, column-vector convention: `v' = M * v`, `m[c][r]`. Uploads to GLSL with no transpose. |
| Winding | Counter-clockwise is front-facing, in NDC. |
| Angles | Radians. |
| Scale | **Uniform only.** `Node3D::set_scale` takes one float. Portal warps only ever produce uniform scale, and the renderer, the physics and the normal transform all depend on it. |

## Clip space

One clip space for both backends: **x, y in [−1, 1] with +Y up, depth in
[0, 1] reversed** — the near plane maps to 1 and the far plane to 0.

* Vulkan has [0, 1] depth natively and +Y down; a **negative-height
  viewport** turns Y back up.
* OpenGL is put into the same space with
  `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`.

So one projection matrix drives both, no shader contains a per-backend
branch, and neither renderer can quietly disagree with the other about
which way is up.

**Winding follows from this.** Vulkan's framebuffer is Y-down, which
reverses a triangle's apparent winding; the negative viewport height
reverses it back. The two cancel, so `VK_FRONT_FACE_COUNTER_CLOCKWISE`
is correct and no flip is applied. Flipping anyway culls every front
face and draws every back face — the screen is not blank, it is inside
out.

## Depth

Reverse-Z, so:

* depth compare is `GREATER_EQUAL`, not `LESS_EQUAL`;
* depth clears to **0.0**, not 1.0;
* the depth-stencil format is **`D32_SFLOAT_S8_UINT`** and an adapter
  without it is rejected. Float depth is what makes reverse-Z pay;
  stencil is what makes portals possible; that format is the only one
  that is both.

Measured, in `tests/test_math`: two surfaces a centimetre apart at a
kilometre are separated by **138 float ULPs** under reverse-Z and by
**0** under the standard mapping — the same float. That is the whole
argument for it.

`Projection::perspective_infinite` has no far plane at all, which
reverse-Z makes *better* rather than worse, since precision is set by
the near plane alone. It is the right default for terrain.

## The oblique near plane

`Projection::with_oblique_near(plane)` replaces the near plane with an
arbitrary one given in view space, keeping the half-space where
`dot(plane, (p, 1)) >= 0`. The x and y rows are untouched, so the image
is unchanged and only the near side of it is cut away.

The published derivation (Lengyel 2005) assumes OpenGL's [−1, 1] depth,
so Manifold derives it again from the clip condition. See the comment in
`src/core/math/projection.h`. It works for off-axis frusta, for an
already-oblique projection (a portal seen through a portal) and for
orthographic (a shadow cascade clipped to a portal), because row 3 is
read from the matrix rather than assumed.

## Shaders

One source. `src/render/shaders/*.glsl`, GLSL 4.50 with Vulkan
semantics, each file holding every stage separated by `#pragma stage`.
The build produces SPIR-V for Vulkan with `glslangValidator` and then
GLSL 460 for OpenGL with `spirv-cross`, **from that same SPIR-V** — so
the two backends cannot drift.

Bindings are **globally unique**: `binding = set * 8 + slot`. Vulkan
reads the (set, binding) pair; OpenGL only gets the binding, because
cross-compiling flattens the sets away. Unique by construction means the
flattening cannot collide.

| set | purpose | bindings |
|---|---|---|
| 0 | per frame — sun, time, shadow atlas | 0–7 |
| 1 | per view — camera, projection | 8–15 |
| 2 | per material | 16–23 |
| 3 | per draw | 24–30 |
| — | push constants (OpenGL emulates with a UBO) | 31 |

Push constants are capped at **96 bytes used of a 128-byte budget** —
128 is what Vulkan guarantees. There is deliberately no normal matrix in
the block: scale is uniform, so the normal basis is the model matrix's
own 3×3 with the scale divided out.

## Images

`Device::read_texture` returns rows **top-down** on both backends. They
do not agree natively — OpenGL stores bottom-up, Vulkan top-down — so
the OpenGL backend flips on the way out. One stated order, or a
screenshot is upside down on one renderer.

## Viewports and scissors

Stated **from the top-left**, y increasing downwards, height positive.
That matches the screen rectangles the renderer computes for portals
and the row order `read_texture` returns.

Neither backend takes it in that form naively, and the conversions are
not the ones you would guess:

* **Vulkan** gets a **negative height** viewport, which is what puts
  +Y up. Its scissor is already top-left.
* **OpenGL** gets both **unchanged**. The instinct is to flip y —
  OpenGL measures window coordinates from the bottom — but this
  backend runs with `glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`,
  which moves the origin of the window coordinate system itself. A
  rectangle already given from the top-left is already correct, and
  flipping it puts the scissor at `height - y - h`: its reflection.

**Nothing catches a wrong flip until a rectangle is asymmetric in y.**
A full-screen viewport is its own reflection, and so is a full-screen
scissor, so every test the engine had passed while both backends
flipped rectangles they should not have — until the portal renderer
scissored a recursion to part of the screen and its contents were
sliced off. `tests/test_backend_parity` now sets an off-centre
viewport and an off-centre scissor and reads back which pixels they
kept.

## Shadows

A shadow map is a depth buffer, so it has the depth buffer's
conventions and no others: reverse-Z, cleared to **0**, compared with
**GREATER_EQUAL**. The comparison sampler in `SamplerCache::shadow` is
set the same way.

It is also a **render target**, so sampling it needs v flipped —
`uv = vec2(ndc.x, -ndc.y) * 0.5 + 0.5` — for the same reason the
tonemap pass flips when it samples the HDR target: row 0 of a target
is its top, while `ndc.y = +1` is the top of the picture.

Three things follow from the engine being a portal engine:

* **Cascades are fitted to every view the frame will draw**, not to
  the camera alone. `Renderer::gather_views` walks the portal
  recursion first, without drawing, and the cascade boxes cover the
  union. Fit them to the main frustum and everything seen through a
  portal is lit by a cascade meant for somewhere else.
* **The light is uploaded as an ordinary view.** A cascade goes
  through the same `ViewData` block a camera uses, so the matrix that
  rendered the map and the matrix that samples it are the same object
  and cannot drift.
* **Back faces are culled, not front faces.** The usual advice is the
  opposite, and it is right for closed watertight geometry and wrong
  for a room, which is a box turned inside out. Culling front faces
  there records the ceiling across the room's whole footprint and the
  interior goes black. Acne is handled by the normal-offset bias in
  `mesh.glsl`, which moves the lookup rather than the stored depth and
  so cannot detach a shadow from its caster.
