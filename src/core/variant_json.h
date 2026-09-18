// Warren -- Variant to JSON and back.
//
// Three callers want this: the agent protocol, which speaks JSON to
// the outside; the procedural shape language, which is JSON that a
// script hands over as a dict; and anything that writes a Variant to
// a file a person will read.
#pragma once

#include <string>

#include "core/json.h"
#include "core/variant.h"

namespace wr {

// Always succeeds. Objects become {"object":"<class>","path":"..."}
// rather than being expanded, because a node's properties include
// other nodes and expanding them walks the whole tree.
Json variant_to_json(const Variant &v);

// LIBERAL ON THE WAY IN.
//
// A vector may arrive as {"x":1,"y":2,"z":3} or as [1,2,3]; a colour
// as either of those, or as "#ff8800", or as a name. A transform may
// arrive as {"position":...,"rotation":...,"scale":...} even though
// none of those three is how one is stored, because that is how a
// caller thinks about placing something. A single number where a
// vector is wanted fills every component.
//
// `want` is the type the target actually has, from reflection. Nil
// means take whatever the JSON suggests. Returns false and fills
// `error` when the shape cannot be made to fit, which is a real
// answer the caller can act on rather than a silently wrong value.
// `current` seeds the components the JSON does not mention, so that
// {"y": 2} on a position moves it up and leaves x and z where they
// were. Without it a partial object silently zeroes the rest, which
// is the kind of quiet wrongness that costs an afternoon.
bool json_to_variant(const Json &j, VType want, Variant *out,
                     std::string *error, const Variant *current = nullptr);

// THE OTHER DIRECTION, for a script.
//
// A Python dict describing a shape is a Variant before it is
// anything else, and the shape parser reads JSON. Rather than a
// second parser that takes Dicts and drifts away from the first,
// the Dict becomes JSON and there is one parser. Objects in the
// Variant have no JSON form and become null.
Json json_from_variant(const Variant &v);

// The type as the schema names it: "vec3", "float", "Node3D".
std::string type_label(VType t, const std::string &class_name = "");

}  // namespace wr
