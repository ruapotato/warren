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

## Punctual lights

Clustered: a grid of `16 x 9 x 24` froxels — tiles in x and y,
exponential slices in z — each holding up to 8 light indices. A
fragment looks up its own froxel and shades against those.

**The grid is per view, not per frame.** A portal view is a different
camera looking at different geometry through the same pixels, so its
froxels contain different lights. Every view's grid lives in one
buffer end to end and each view carries the index of where its block
starts, which is why no dynamic storage-buffer offsets are needed.

Two things have to agree between the binder and the shader or lights
flicker as the camera turns, and both are easy to get backwards:

* **Tile y counts from the TOP**, matching `gl_FragCoord.y`, which is
  measured from the top on both backends.
* **The slice mapping** is `slice = log2(z) * scale + bias` with
  `scale = CLUSTER_Z / log2(far/near)`. The binder solves it from a
  boundary table rather than from the log, because a light behind the
  eye still lights what is in front of it and `log2` of a negative
  depth is not a number.

The froxel bounds come from `Projection::get_extents_at`, which reads
rows 0, 1 and w and so is unaffected by an oblique near plane — the
same property the shadow cascades rely on.

Lights are gathered in `Renderer::collect`, and a `DirectionalLight3D`
in the tree now overrides the renderer's own sun fields. The first one
wins; a second sun is a scene mistake, not a feature.

## The environment

Baked from `sky_radiance` in `sky_model.glsl`, which `sky.glsl` also
draws the backdrop with. Two copies of that gradient would drift the
first time anyone tuned one, and a world lit slightly wrong for the
sky behind it is not a bug anybody traces to a duplicated shader. The
reflectance model is shared the same way, in `brdf.glsl`, between the
surface shader and the prefilter.

A shader file with **no `#pragma stage`** is an include, not a
program, and the build skips it on that basis rather than by its name.

`cube_direction(face, st)` maps a texel to the direction it looks in,
with `st` measured from the **top left** like any other render target.
Twelve signs, and one wrong lights the world by a sky reflected in an
axis — which looks entirely plausible. `tests/test_ibl` renders a
white sphere lit only by the environment, recovers the surface normal
at each pixel from its position on the sphere, and requires brightness
to rise with the cosine to the sun at every step. Flipping one face
breaks the ordering while leaving the average over the sphere
unchanged, which is why the average alone is not the test.

The two backends do **not** agree to the bit here, and this is the one
place in the engine where they do not. Cubemap mip generation is a
driver's own box filter on OpenGL and a chain of blits on Vulkan, the
prefilter reads those mips, and the difference is largest on the
shiniest surface in the frame: mean 0.03/255 and worst 3 on the test's
sphere, mean 0.08 and worst 20 in the portals demo. It must stay a
rounding difference — smooth, small and everywhere — never a shape.
