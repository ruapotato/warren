# Known issues

Things that are wrong, reproduced, and not yet fixed. Written down
rather than remembered, because a bug nobody can reproduce on demand
is a bug that gets argued about instead of fixed.

---

## Exposure above 1.0 can produce an empty frame

**Reproduce**

```
# in src/render/renderer.h, RenderSettings:
#     float exposure = 1.15f;      (anything > 1.0)
ninja -C build
./build/bin/warren --demo nav --frames 12 --shot /tmp/a.png --shot-frame 8
```

Every pixel of `/tmp/a.png` comes back `(0, 0, 0, 0)`. At
`exposure = 1.0` the same command produces the scene. The threshold
is sharp: `0.99` and `1.0` are fine, `1.001` is not.

**What is known**

- Not the Environment proxy or the script bridge: it reproduces with
  the value compiled in as the default and no script running.
- Not a driver: Vulkan and OpenGL fail identically.
- Not the screenshot picking the wrong image: the swapchain index
  logged by the render and by the capture are the same one.
- Not the tonemap pass failing to run. Giving that pass a green
  `LoadOp::Clear` shows green everywhere, so the pass executes and
  the full-screen triangle contributes nothing.
- Not the tonemap shader's arithmetic. A debug build of it that
  writes `vec4(push.params.x * 0.5, 0.25, 0, 1)` -- ignoring the HDR
  texture entirely -- still writes nothing at 1.15 and writes the
  expected colour at 1.0.
- Clamping only the PUSHED value, with `settings_.exposure` left at
  1.15, renders correctly. So the trigger is the float that reaches
  the GPU.
- **Not universal across scenes.** `--demo portals` renders correctly
  at 1.15. `--demo nav` and the ROTGRAVE town do not. Whatever the
  cause is, it involves the scene as well as the value, which is the
  thread to pull next.

The shape of it -- a draw that produces no fragments, depending on a
uniform value and on what else is in the scene -- points at a NaN or
an infinity somewhere in the HDR buffer that the multiply tips over
an edge, rather than at anything in the tonemap itself. That is a
guess and is labelled as one.

**Workaround**

Leave `exposure` at 1.0 and light the scene with `sun_energy`,
`ambient_energy` and `env_intensity`, which are unaffected. ROTGRAVE
does this.

---

## A doorway with a step in it does not connect

**Reproduce**

`tests/test_meshbuild.cpp`, the "a doorway with a step in it" case.
Two rooms joined by a 2.6 m doorway, the inner floor raised by
`step`, an agent that can climb 0.45 m, 0.2 m cells and 0.15 m cell
height:

| step | connects |
|---|---|
| 0.00 m | yes |
| 0.15 m | yes |
| 0.30 m | **no** |
| 0.45 m | **no** |

Both failing steps are well within the agent's climb, and every
staircase in `test_nav` -- which climbs 2.4 m in eight treads --
works. The difference is that a staircase's treads end up in one
region, while a doorway is a narrow place, so the watershed cuts
there and the two sides are different regions.

**What is known**

Traced through the bake stages on the minimal world:

- The compact field is the same: nine regions, no unassigned spans,
  the same inside and outside region ids.
- At 0.00 and 0.15 both regions put a contour vertex on the same
  two cell corners in the doorway, each naming the other as its
  neighbour, and the polygons share two edges.
- At 0.30 the outer region has no contour vertex in the doorway at
  all and there are no cross-region edges. Four polygons fewer.
- The cause is upstream of the contours. Walking the cells across
  the threshold shows the doorway's own column has **no walkable
  floor span** at 0.30 -- the only span left is the ceiling, five
  metres up. Before erosion, so the ledge filter or the span
  rejection is dropping it, not the erosion.

So the bake closes the doorway one cell at a time as the step grows,
and the contour stage is only reporting it.

**Workaround**

Keep thresholds flush, or within one cell of `cell_height`.
ROTGRAVE does: `Design._resolve_levels` snaps a building's floor to
the ground immediately outside it when the two are close, so the
church sits at its churchyard's height rather than 0.3 m above it.
