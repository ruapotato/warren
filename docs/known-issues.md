# Known issues

Things that are wrong, reproduced, and not yet fixed. Written down
rather than remembered, because a bug nobody can reproduce on demand
is a bug that gets argued about instead of fixed.

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

**A near miss worth recording.** This defect was blamed, for some
time, for the town's zones being cut off from one another -- the
symptom matched, several of the breaks were at height changes, and
the pruning duly deleted a quarter of the map. It was not the cause.
The cause was procedural clutter: parked wrecks placed across
doorways, which seal a room as effectively as a wall and look like
scenery. Keeping the clutter clear of doors connected every zone and
every door in the plan, with the pruning seeded from the starting
zones -- the strict and correct semantics -- and no workaround.

Worth recording because the wrong diagnosis was available, plausible,
and had a real defect behind it.

---

# Fixed, and worth keeping

The reasoning that missed, as well as the reasoning that landed. A
wrong diagnosis that was plausible is worth more written down than
forgotten, because the next person will reach for it too.

## Exposure above 1.0 produced an empty frame

**What it was.** The tonemap pass was built with `fullscreen.glsl`'s
vertex stage, which writes `push.params.x` into `gl_Position.z` --
that shader exists partly to clear depth inside a stencilled region
by drawing a triangle at a chosen depth, and the depth is where it
takes it from. The tonemap's own fragment stage reads that same push
slot as the exposure. So the exposure was also the clip-space Z, and
with `w = 1.0` anything above 1.0 put all three vertices outside the
clip volume and the triangle was discarded before rasterising.

That is the whole of it, and it explains the part that looked
strangest: the threshold was *exactly* 1.0, because that is exactly
where the clip volume ends. `0.99` drew, `1.001` did not.

**What was guessed and was wrong.** A NaN or an infinity in the HDR
buffer that the multiply tipped over an edge -- the note said at the
time that this was a guess, and it was the wrong one. It fitted the
evidence that had been gathered (a draw producing no fragments,
depending on a uniform and seemingly on the scene) and it sent the
search into the HDR pass, which was the one place the bug was not.

Two observations should have ended it sooner. A debug tonemap
fragment shader that ignored the HDR texture entirely *still* drew
nothing, which rules out everything downstream of the vertex stage
and was recorded without being followed. And the threshold sat on a
round number that means something specific in clip space. "Not
universal across scenes" was the observation that did the most harm:
it was never nailed down, it argued for a data-dependent cause, and
it pointed away from a pipeline that is identical in every scene.

**The fix.** `renderer.cpp` gives the tonemap pass its own vertex
stage, which `tonemap.glsl` already declared. Two passes sharing a
vertex shader while disagreeing about what a push constant means is
the actual defect; the empty frame was a symptom.
