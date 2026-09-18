// Warren -- bones, and the pose of a set of them.
//
// A SKELETON IS A RESOURCE; A POSE IS NOT.
//
// Twenty shamblers in a street are twenty poses and ONE skeleton. The
// skeleton is what the mesh was bound against -- the bone names, who
// each one's parent is, where each one rests and the inverse of that
// rest -- and none of it changes while the game runs. The pose is
// where the bones are this instant, and there is one per body. Putting
// them in the same object is how an engine ends up rebuilding a
// skeleton per character per frame.
//
// PARENTS COME FIRST. A bone's index is always greater than its
// parent's, which is enforced at build time. That one invariant turns
// "resolve the hierarchy" from a recursive walk with a visited set
// into a single forward loop, which is the difference between skinning
// twenty bodies being free and being a profile entry.
#pragma once

#include <string>
#include <vector>

#include "core/math/transform.h"
#include "core/object.h"
#include "resource/resource.h"

namespace wr {

class Skeleton : public Resource {
    WR_CLASS(Skeleton, Resource)

public:
    struct Bone {
        std::string name;
        // -1 for a root. Always less than this bone's own index.
        int parent = -1;
        // Where the bone sits relative to its parent when nothing is
        // animating it. A clip that touches only the arms leaves every
        // other bone here.
        Transform3D rest;
        // Model space at bind time, inverted: the matrix that takes a
        // vertex from the mesh's own space into this bone's. glTF ships
        // it; a rig built in code computes it from the rests.
        Transform3D inverse_bind;
    };

    std::vector<Bone> bones;

    int find(const std::string &name) const;
    int count() const { return int(bones.size()); }
    bool empty() const { return bones.empty(); }

    // Add a bone and return its index. Refuses a parent that is not
    // already in -- see the note above about ordering.
    int add(const std::string &name, int parent, const Transform3D &rest);

    // Inverse binds from the rests, for a rig that was built rather
    // than imported.
    void compute_inverse_binds();

    // The rest pose in model space, which is what a retargeter measures
    // against and what an editor draws.
    Transform3D global_rest(int bone) const;

    // TRUE IF EVERY PARENT COMES BEFORE ITS CHILD. Checked on import,
    // because a file that breaks it would skin subtly wrong rather
    // than fail, and "subtly wrong" is an afternoon.
    bool ordered() const;
    // Put it in that order, remapping parents. Returns the mapping from
    // old index to new, so a mesh's joint indices can follow.
    std::vector<int> sort_hierarchically();
};

// WHERE THE BONES ARE NOW.
//
// One per animated body. `local` is what a clip writes and what a
// script pokes; `global` and `skin` are derived from it by `resolve`.
// Keeping the derived arrays here rather than recomputing into a
// scratch buffer means the renderer can upload straight out of the
// pose.
struct Pose {
    const Skeleton *skeleton = nullptr;
    std::vector<Transform3D> local;
    std::vector<Transform3D> global;
    // global * inverse_bind, which is what the vertex shader wants.
    std::vector<Transform3D> skin;

    void bind(const Skeleton *s);
    void reset();                     // back to the rest pose
    // One forward pass: local -> global -> skin. See the ordering note.
    void resolve();
    bool valid() const { return skeleton != nullptr && !local.empty(); }
    int count() const { return int(local.size()); }
};

}  // namespace wr
