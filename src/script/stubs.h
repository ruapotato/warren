// Warren -- type stubs for the Python binding.
//
// The binding is generated from ClassDB at run time, which means an
// editor has nothing to read: no .py file declares mf.Node3D. This
// writes the .pyi that closes that gap, from the same table. It needs
// no interpreter -- it is text generation over the class registry --
// so it works in a build with Python switched off, and the stubs
// cannot drift from the engine because there is nothing to keep in
// sync.
#pragma once

#include <string>

namespace wr {

// The whole `warren` module as a PEP 484 stub. Includes any classes
// a plugin registered, so generate it after plugins load.
std::string python_stubs();
// Writes it, making parent directories. False (and a logged error) if
// the file could not be written.
bool write_python_stubs(const std::string &path);

}  // namespace wr
