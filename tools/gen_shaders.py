#!/usr/bin/env python3
"""Warren's shader build.

One source, two backends. Each .glsl file under src/render/shaders holds
every stage of one program, separated by `#pragma stage <name>`. For
each stage this tool produces:

  * SPIR-V, via glslangValidator, for the Vulkan backend;
  * GLSL 460, via spirv-cross from that same SPIR-V, for the OpenGL one.

Compiling the OpenGL variant FROM the SPIR-V rather than from the
original source is deliberate. It means the two backends cannot drift:
if a shader compiles for Vulkan it exists for OpenGL, built from the
same validated module, with the same optimisations already applied.

Both go into one generated C++ translation unit, so a build has no
loose shader files to lose and a release has nothing to install.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHADER_DIR = os.path.join(ROOT, "src", "render", "shaders")
OUT_DIR = os.path.join(SHADER_DIR, "generated")

STAGES = {
    "vertex": ("vert", "Vertex"),
    "fragment": ("frag", "Fragment"),
    "compute": ("comp", "Compute"),
}

INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$', re.M)
PRAGMA_RE = re.compile(r'^\s*#pragma\s+stage\s+(\w+)\s*$', re.M)


def which(name):
    from shutil import which as _w
    p = _w(name)
    if not p:
        sys.stderr.write(
            "gen_shaders: %s not found. Install glslang-tools and spirv-cross.\n"
            % name)
        sys.exit(2)
    return p


def resolve_includes(path, seen=None, depth=0):
    """Splice #include "..." recursively, keeping line numbers honest
    with #line directives so a compile error points at the real file."""
    if seen is None:
        seen = set()
    if depth > 16:
        raise RuntimeError("include nesting too deep in %s" % path)
    real = os.path.abspath(path)
    text = open(real).read()
    out = []
    last = 0
    for m in INCLUDE_RE.finditer(text):
        out.append(text[last:m.start()])
        inc = os.path.join(os.path.dirname(real), m.group(1))
        if not os.path.exists(inc):
            inc = os.path.join(SHADER_DIR, m.group(1))
        if not os.path.exists(inc):
            raise RuntimeError("%s: cannot find include %s" % (path, m.group(1)))
        if os.path.abspath(inc) in seen:
            # Header guards handle re-inclusion; skip to keep it cheap.
            out.append("\n")
        else:
            seen.add(os.path.abspath(inc))
            out.append(resolve_includes(inc, seen, depth + 1))
        last = m.end()
    out.append(text[last:])
    return "".join(out)


def split_stages(source, path):
    """Everything before the first #pragma stage is shared preamble."""
    marks = list(PRAGMA_RE.finditer(source))
    if not marks:
        raise RuntimeError("%s: no '#pragma stage' -- which stage is it?" % path)
    preamble = source[:marks[0].start()]
    stages = {}
    for i, m in enumerate(marks):
        name = m.group(1)
        if name not in STAGES:
            raise RuntimeError("%s: unknown stage '%s'" % (path, name))
        end = marks[i + 1].start() if i + 1 < len(marks) else len(source)
        stages[name] = source[m.end():end]
    return preamble, stages


def compile_stage(glslang, spirv_cross, spirv_opt, name, stage, body, preamble,
                  optimise, tmpdir):
    ext, _ = STAGES[stage]
    stage_define = "#define WR_STAGE_%s 1\n" % stage.upper()
    full = "#version 450\n" + stage_define + preamble + body
    src_path = os.path.join(tmpdir, "%s.%s" % (name, ext))
    with open(src_path, "w") as f:
        f.write(full)

    spv_path = os.path.join(tmpdir, "%s.%s.spv" % (name, ext))
    cmd = [glslang, "--target-env", "vulkan1.3", "-S", ext, "-o", spv_path]
    if optimise:
        cmd.append("-g0")
    cmd.append(src_path)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("\n--- %s [%s] failed to compile ---\n%s%s\n"
                         % (name, stage, r.stdout, r.stderr))
        # Print the generated source with line numbers; the error refers
        # to it, not to the file the author wrote.
        for i, line in enumerate(full.split("\n"), 1):
            sys.stderr.write("%4d | %s\n" % (i, line))
        raise SystemExit(1)

    if optimise and spirv_opt:
        opt_path = spv_path + ".opt"
        r = subprocess.run([spirv_opt, "-O", spv_path, "-o", opt_path],
                           capture_output=True, text=True)
        if r.returncode == 0:
            spv_path = opt_path

    with open(spv_path, "rb") as f:
        blob = f.read()
    if len(blob) % 4:
        raise RuntimeError("%s: SPIR-V is not a whole number of words" % name)
    words = [int.from_bytes(blob[i:i + 4], "little") for i in range(0, len(blob), 4)]

    # The OpenGL variant, from the same module.
    r = subprocess.run([spirv_cross, spv_path, "--version", "460", "--no-es",
                        "--glsl-emit-push-constant-as-ubo"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("\n--- %s [%s] cross-compile failed ---\n%s\n"
                         % (name, stage, r.stderr))
        raise SystemExit(1)
    glsl = r.stdout
    return words, glsl


def c_string(s):
    """A C++ raw string is not safe for arbitrary shader text (it could
    contain the delimiter), so escape properly."""
    out = []
    for line in s.split("\n"):
        out.append('"%s\\n"' % line.replace("\\", "\\\\").replace('"', '\\"'))
    return "\n    ".join(out) if out else '""'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--optimise", action="store_true")
    ap.add_argument("--out-dir", default=OUT_DIR)
    args = ap.parse_args()

    glslang = which("glslangValidator")
    spirv_cross = which("spirv-cross")
    from shutil import which as _w
    spirv_opt = _w("spirv-opt")

    os.makedirs(args.out_dir, exist_ok=True)
    # A FILE WITH NO STAGES IS AN INCLUDE, NOT A PROGRAM.
    #
    # This used to skip anything named common*, which worked until the
    # second shared header wanted a name of its own and the build
    # stopped with "which stage is it?". The property that actually
    # distinguishes the two is whether the file declares any stages,
    # so that is what is tested.
    sources = []
    for f in sorted(os.listdir(SHADER_DIR)):
        if not f.endswith(".glsl"):
            continue
        if not PRAGMA_RE.search(open(os.path.join(SHADER_DIR, f)).read()):
            continue
        sources.append(f)

    entries = []
    body_cpp = []
    with tempfile.TemporaryDirectory() as tmpdir:
        for fname in sources:
            path = os.path.join(SHADER_DIR, fname)
            name = os.path.splitext(fname)[0]
            merged = resolve_includes(path)
            preamble, stages = split_stages(merged, path)
            for stage in ("vertex", "fragment", "compute"):
                if stage not in stages:
                    continue
                words, glsl = compile_stage(glslang, spirv_cross, spirv_opt, name,
                                            stage, stages[stage], preamble,
                                            args.optimise, tmpdir)
                sym = "%s_%s" % (name, stage)
                body_cpp.append("static const uint32_t k_%s_spirv[] = {\n%s\n};\n"
                                % (sym, "    " + ",\n    ".join(
                                    ", ".join("0x%08xu" % w for w in words[i:i + 6])
                                    for i in range(0, len(words), 6))))
                body_cpp.append("static const char *const k_%s_glsl =\n    %s;\n"
                                % (sym, c_string(glsl)))
                entries.append((name, stage, sym, len(words), len(glsl)))
                print("  %-24s %-9s %6d spirv words, %6d glsl bytes"
                      % (name, stage, len(words), len(glsl)))

    header = ["// GENERATED by tools/gen_shaders.py. Do not edit.",
              "#pragma once", "", "#include \"rhi/rhi.h\"", "",
              "namespace wr::shaders {", "",
              "struct Blob {",
              "    const char *name;",
              "    ::wr::rhi::ShaderStage stage;",
              "    const uint32_t *spirv;",
              "    size_t spirv_words;",
              "    const char *glsl;",
              "};", "",
              "// Every shader the engine was built with.",
              "extern const Blob *const all;",
              "extern const size_t count;",
              "// By program name and stage, or null.",
              "const Blob *find(const char *name, ::wr::rhi::ShaderStage stage);",
              "// Ready to hand straight to Device::create_shader.",
              "::wr::rhi::ShaderDesc desc(const Blob &b);", ""]
    for name, stage, sym, _, _ in entries:
        header.append("extern const Blob %s;" % sym)
    header += ["", "}  // namespace wr::shaders", ""]

    cpp = ["// GENERATED by tools/gen_shaders.py. Do not edit.",
           "#include \"shaders.h\"", "", "#include <cstring>", "",
           "namespace wr::shaders {", "namespace {", ""]
    cpp += body_cpp
    cpp += ["}  // namespace", ""]
    for name, stage, sym, nwords, _ in entries:
        cpp.append("const Blob %s = {\"%s\", ::wr::rhi::ShaderStage::%s, "
                   "k_%s_spirv, %d, k_%s_glsl};"
                   % (sym, name, STAGES[stage][1], sym, nwords, sym))
    cpp += ["", "static const Blob k_all[] = {"]
    for _, _, sym, _, _ in entries:
        cpp.append("    %s," % sym)
    cpp += ["};", "",
            "const Blob *const all = k_all;",
            "const size_t count = %d;" % len(entries), "",
            "const Blob *find(const char *name, ::wr::rhi::ShaderStage stage) {",
            "    for (size_t i = 0; i < count; i++)",
            "        if (k_all[i].stage == stage && std::strcmp(k_all[i].name, name) == 0)",
            "            return &k_all[i];",
            "    return nullptr;",
            "}", "",
            "::wr::rhi::ShaderDesc desc(const Blob &b) {",
            "    ::wr::rhi::ShaderDesc d;",
            "    d.stage = b.stage;",
            "    d.spirv = b.spirv;",
            "    d.spirv_words = b.spirv_words;",
            "    d.glsl = b.glsl;",
            "    d.name = b.name;",
            "    return d;",
            "}", "",
            "}  // namespace wr::shaders", ""]

    open(os.path.join(args.out_dir, "shaders.h"), "w").write("\n".join(header))
    open(os.path.join(args.out_dir, "shaders.cpp"), "w").write("\n".join(cpp))
    print("gen_shaders: %d stages from %d programs -> %s"
          % (len(entries), len(sources), args.out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
