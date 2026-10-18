#!/usr/bin/env python3
"""Build this engine's menu pk3 without shipping id Software's menu files.

The engine's menus are edited versions of RTCW's own menus (game data, not GPL). Git therefore stores only
*deltas*: for every derived file, the lines we wrote plus the line ranges of the original to replace. Passages
that came from an original (moved or reused blocks, even from another menu) are stored as *references* to line
ranges of the user's own RTCW paks, never as text. Only braces/blank lines and one-line property assignments whose
indentation differs from the original (`name back`, `border 0`) stay literal. The originals are read from the
user's paks, verified by SHA-256, and the delta is applied to produce the full file.

Engine layer (this repo): SP/menus/deltas/*.delta on RTCW's original menus -> SP/menus/ui/ (generated, git-ignored,
editable) -> SP/menus/sp_zz_engine_menus.pk3 (with this repo's ui/menudef.h, whose FEEDER_* ids RTCW's copy lacks).
The pk3 goes in main/; the `sp_` prefix sorts it after sp_pak4 so it overrides RTCW's menus (do not use `zz_`: the
engine renames those to `sp_`). It holds only the files the engine changes, so everything else still comes from
RTCW's paks. A mod can build its own layer on top: import this module and use a Layer with base=ENGINE.

  menudelta.py build            generate missing/outdated ui files from the deltas, then pack the pk3
  menudelta.py build --export   also refresh deltas from locally edited ui files (developer workflow)
  menudelta.py export           only refresh deltas

Delta format (<layer deltas dir>/<name>.delta):
  line 1:  #menu-delta v2 base=<path> sha256=<normalised base> [layer=engine]
           without layer=engine the base is an original inside the paks; with it, the base is the engine-layer
           result of that path (and the sha is checked against that)
  #src <path inside paks> sha256=<...>     one line per original that is referenced below (besides an original base)
  hunks:   @@ <start> <end>     replace base lines [start, end) (0-based) with what follows:
           +<new line>          a literal line of ours
           =<path> <a> <b>      lines [a, b) (0-based) of that original (base or #src file)
"""
import argparse, difflib, hashlib, json, os, sys, zipfile
from pathlib import Path

SP = Path(__file__).resolve().parent.parent
HERE = SP / "menus"
PAKS = ["pak0.pk3", "sp_pak1.pk3", "sp_pak2.pk3", "sp_pak3.pk3", "sp_pak4.pk3"]  # later overrides earlier
MIN_RUN = 3       # a run of only trivial lines (braces, blanks) shorter than this stays literal
MIN_REUSE = 0.15  # share of our non-trivial lines that also exist in a base => treat as derived
FORMATS = ("#menu-delta", "#codmod-delta")  # the old name is still read


class Layer:
    """A set of deltas (`deltas` dir) generated into `ui`. `base` = the layer whose output layer=engine deltas edit;
    `txt` = list files (besides .menu) the layer may carry as deltas on RTCW's."""
    def __init__(self, name, ui, deltas, base=None, txt=()):
        self.name, self.ui, self.deltas, self.base, self.txt = name, Path(ui), Path(deltas), base, tuple(txt)


ENGINE = Layer("engine", HERE / "ui", HERE / "deltas", txt=("menus.txt", "ingame.txt"))
ENG_PK3 = HERE / "sp_zz_engine_menus.pk3"
MENUDEF = SP / "main" / "ui" / "menudef.h"


def die(msg):
    sys.exit(f"menus: {msg}")


def pak_dirs(extra=()):
    dirs = []
    if os.environ.get("RTCW_DATA"):
        dirs.append(Path(os.environ["RTCW_DATA"]))
    dirs += [Path(d) for d in extra]
    dirs.append(SP / "build" / "run" / "main")
    home = Path.home()
    dirs += [home / ".local/share/Steam/steamapps/common/Return to Castle Wolfenstein/Main",
             home / ".steam/steam/steamapps/common/Return to Castle Wolfenstein/Main",
             home / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Return to Castle Wolfenstein/Main"]
    return dirs


def load_originals(extra_dirs=()):
    """Return {name: normalised text lines} for every ui/* menu text file in the user's paks."""
    found = [d for d in pak_dirs(extra_dirs) if (d / "sp_pak1.pk3").exists()]
    if not found:
        die("RTCW paks not found. Set RTCW_DATA to your '.../Return to Castle Wolfenstein/Main'.")
    d = found[0]
    out = {}
    for pak in PAKS:
        f = d / pak
        if not f.exists():
            continue
        with zipfile.ZipFile(f) as z:
            for n in z.namelist():
                if n.startswith("ui/") and n.count("/") == 1 and n.endswith((".menu", ".txt", ".h")):
                    out[n] = z.read(n).decode("latin-1").replace("\r\n", "\n").replace("\r", "\n").split("\n")
    return out


def sha(lines):
    return hashlib.sha256("\n".join(lines).encode("latin-1")).hexdigest()


def trivial(l):
    return len(l.strip()) < 8


def line_index(originals):
    """{line: [(name, i), ...]} over every original, for finding reusable runs."""
    idx = {}
    for name, lines in originals.items():
        for i, l in enumerate(lines):
            idx.setdefault(l, []).append((name, i))
    return idx


def split_runs(new, originals, idx, base_name):
    """Greedy: cover `new` with references to original runs (>= MIN_RUN lines) and literal lines."""
    out, i = [], 0
    while i < len(new):
        best = (0, None, 0)
        for name, a in idx.get(new[i], ()):
            src = originals[name]
            n = 0
            while i + n < len(new) and a + n < len(src) and new[i + n] == src[a + n]:
                n += 1
            if n > best[0] or (n == best[0] and name == base_name and best[1] != base_name):
                best = (n, name, a)
        n = best[0]
        if n and (n >= MIN_RUN or not all(trivial(l) for l in new[i:i + n])):
            out.append(f"={best[1]} {best[2]} {best[2] + n}")
            i += n
        else:
            out.append("+" + new[i])
            i += 1
    return out


def make_delta(base_name, base, new, originals=None, idx=None, layer=None):
    sm = difflib.SequenceMatcher(None, base, new, autojunk=False)
    body, used = [], set()
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        body.append(f"@@ {i1} {i2}")
        if originals is None:
            body += ["+" + l for l in new[j1:j2]]
            continue
        segs = split_runs(new[j1:j2], originals, idx, base_name)
        used |= {s[1:].split()[0] for s in segs if s[0] == "="}
        body += segs
    out = [f"{FORMATS[0]} v2 base={base_name} sha256={sha(base)}" + (f" layer={layer}" if layer else "")]
    if originals is not None:
        # an original base is checked as the base; with an engine-layer base every original is a #src
        out += [f"#src {n} sha256={sha(originals[n])}" for n in sorted(used - ({base_name} if not layer else set()))]
    return "\n".join(out + body) + "\n"


def delta_header(text):
    """(base_name, layer or None) of a delta text."""
    hdr = text.split("\n", 1)[0].split()
    layer = hdr[4].split("=", 1)[1] if len(hdr) > 4 else None
    return hdr[2].split("=", 1)[1], layer


def apply_delta(text, originals, engine=None):
    """Return the full file text. `engine` = {path: lines} of the engine layer (for layer=engine bases)."""
    lines = text.split("\n")
    hdr = lines[0].split()
    if hdr[0] not in FORMATS or hdr[1] != "v2":
        die("unknown delta format")
    base_name, layer = delta_header(text)
    base_sha = hdr[3].split("=", 1)[1]
    if layer:
        if engine is None or base_name not in engine:
            die(f"{base_name} is not in the engine layer (needed by a delta with layer={layer})")
        base = engine[base_name]
        if sha(base) != base_sha:
            die(f"engine-layer {base_name} changed since this delta was made. "
                f"Re-export this layer's deltas (build --export) to refresh them.")
        refs = {}
    else:
        if base_name not in originals:
            die(f"{base_name} not found in your RTCW paks")
        base = originals[base_name]
        refs = {base_name: base_sha}
    hunks, cur = [], None
    for l in lines[1:]:
        if l.startswith("#src "):
            _, n, h = l.split()
            refs[n] = h.split("=", 1)[1]
        elif l.startswith("@@ "):
            _, a, b = l.split()
            cur = [int(a), int(b), []]
            hunks.append(cur)
        elif l.startswith("+"):
            cur[2].append(l[1:])
        elif l.startswith("="):
            n, a, b = l[1:].split()
            if n not in refs:
                die(f"delta references {n} without a #src line")
            cur[2] += originals[n][int(a):int(b)]
    for n, want in refs.items():
        if n not in originals:
            die(f"{n} not found in your RTCW paks")
        if sha(originals[n]) != want:
            die(f"{n} in your paks differs from the version these menus were built against "
                f"(expected the RTCW 1.41 data). Cannot rebuild menus.")
    res, pos = [], 0
    for a, b, new in hunks:
        res += base[pos:a] + new
        pos = b
    res += base[pos:]
    return "\n".join(res)


def best_base(new, originals):
    """Pick the original that shares the most non-trivial lines with `new`; None if below threshold."""
    nt = [l for l in new if not trivial(l)]
    if not nt:
        return None, 0.0
    best = (None, 0.0)
    for name, base in originals.items():
        if not name.endswith(".menu"):
            continue
        bs = {}
        for l in base:
            if not trivial(l):
                bs[l] = bs.get(l, 0) + 1
        used, hit = {}, 0
        for l in nt:
            if used.get(l, 0) < bs.get(l, 0):
                used[l] = used.get(l, 0) + 1
                hit += 1
        r = hit / len(nt)
        if r > best[1]:
            best = (name, r)
    return best if best[1] >= MIN_REUSE else (None, best[1])


def pick_base(new, originals):
    """Among the originals sharing the most lines with `new`, take the one giving the smallest delta."""
    name, r = best_base(new, originals)
    if name is None:
        return None
    ranked = sorted(((n, 0) for n in originals if n.endswith(".menu")), key=lambda t: t[0])
    nt = {l for l in new if not trivial(l)}
    scored = sorted(ranked, key=lambda t: -len(nt & {l for l in originals[t[0]] if not trivial(l)}))[:5]
    return min((n for n, _ in scored), key=lambda n: make_delta(n, originals[n], new).count("\n+"))


def read_state(layer):
    try:
        return json.loads((layer.ui / ".menus-state.json").read_text())
    except Exception:
        return {}


def write_state(layer, state):
    layer.ui.mkdir(parents=True, exist_ok=True)
    (layer.ui / ".menus-state.json").write_text(json.dumps(state, indent=1))


def fhash(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def delta_files(layer):
    return sorted(layer.deltas.glob("*.delta")) if layer.deltas.exists() else []


def read_lines(f):
    return f.read_text(encoding="latin-1").replace("\r\n", "\n").split("\n")


def layer_texts(layer, originals):
    """{ui/<name>: lines} of a base layer, computed from its deltas only (ignores local edits)."""
    return {"ui/" + dp.name[:-6]: apply_delta(dp.read_text(), originals).split("\n") for dp in delta_files(layer)}


def export_layer(layer, originals, idx, eng=None):
    """Refresh the deltas of one layer from its ui dir. `eng` = texts of layer.base (for a layer on top of it).
    Returns the files kept whole in git (no RTCW original resembles them)."""
    layer.deltas.mkdir(parents=True, exist_ok=True)
    state = read_state(layer)
    kept = []
    names = sorted(f for f in (layer.ui.iterdir() if layer.ui.exists() else []) if f.is_file() and not f.name.startswith("."))
    names = [f for f in names if f.name.endswith(".menu") or f.name in layer.txt]
    for f in names:
        new = read_lines(f)
        prev = layer.deltas / (f.name + ".delta")
        key = "ui/" + f.name
        blayer = None
        if eng is not None and key in eng:
            if new == eng[key]:      # identical to the base layer: no delta, copied on build
                if prev.exists():
                    prev.unlink()
                    print(f"export: {prev.name} removed (same as the {layer.base.name} layer)")
                continue
            base_name, blayer, base = key, layer.base.name, eng[key]
        else:
            # prefer the previous base for this file, so the delta stays stable
            base_name = None
            if prev.exists():
                base_name = delta_header(prev.read_text())[0]
            elif key in originals:
                base_name = key  # edited version of the same-named original
            else:
                base_name = pick_base(new, originals)
            base = originals[base_name] if base_name else None
        if base_name is None:
            kept.append(f.name)  # entirely our own file: stays in git as is
            if prev.exists():
                prev.unlink()
            continue
        d = make_delta(base_name, base, new, originals, idx, blayer)
        if not prev.exists() or prev.read_text() != d:
            prev.write_text(d)
            print(f"export: {layer.name}/{prev.name} ({base_name}{' @' + blayer if blayer else ''})")
        state[f.name] = {"delta": hashlib.sha256(d.encode()).hexdigest(), "file": fhash(f)}
    write_state(layer, state)
    return kept


def sync(target, text, src_hash, state, name, export_too, label):
    """Write `text` to target unless the user edited the file locally. Returns nothing; updates state."""
    st = state.get(name)
    if not target.exists() or (st and st["file"] == fhash(target) and st["delta"] != src_hash):
        target.write_text(text, encoding="latin-1", newline="\n")
        state[name] = {"delta": src_hash, "file": fhash(target)}
        print(f"generate: {label}")
    elif not st:
        state[name] = {"delta": src_hash, "file": fhash(target)}
    elif st["file"] != fhash(target) and not export_too:
        print(f"note: {label} has local edits not in its delta (re-export the deltas: build --export)")


def generate_layer(layer, originals, eng=None, export_too=False, passthrough=True):
    """Write the layer's ui files from its deltas. With `eng` (texts of layer.base), base-layer .menu files that have
    no delta in this layer are copied unchanged (`passthrough`)."""
    layer.ui.mkdir(parents=True, exist_ok=True)
    state = read_state(layer)
    for dp in delta_files(layer):
        dtext = dp.read_text()
        name = dp.name[:-6]
        sync(layer.ui / name, apply_delta(dtext, originals, eng), hashlib.sha256(dtext.encode()).hexdigest(),
             state, name, export_too, f"{layer.ui}/{name}")
    if eng is not None and passthrough:
        for name in passthrough_names(layer, eng):
            text = "\n".join(eng["ui/" + name])
            sync(layer.ui / name, text, hashlib.sha256(text.encode()).hexdigest(), state, name, export_too,
                 f"{layer.ui}/{name} (from {layer.base.name} layer)")
    write_state(layer, state)


def passthrough_names(layer, eng):
    """Base-layer .menu files this layer takes unchanged (no delta of its own)."""
    own = {dp.name[:-6] for dp in delta_files(layer)}
    return sorted(k[3:] for k in eng if k.endswith(".menu") and k[3:] not in own)


def pack(out, entries):
    tmp = out.with_suffix(".tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, path in sorted(entries):
            zi = zipfile.ZipInfo(arc, (2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, path.read_bytes())
    tmp.replace(out)
    print(f"built {out} ({len(entries)} entries)")


def engine_gitignore():
    ENGINE.ui.mkdir(parents=True, exist_ok=True)
    (ENGINE.ui / ".gitignore").write_text(
        "# Generated from SP/menus/deltas/ by menudelta.py (derived from RTCW's own menus: not redistributed)\n"
        "/*\n!/.gitignore\n")


def build_engine_pk3():
    # only the files the engine changes or adds, so everything else still comes from RTCW's own paks;
    # plus this repo's menudef.h (GPL): RTCW's copy in pak0 lacks the engine's newer FEEDER_* ids
    eng = [(f"ui/{dp.name[:-6]}", ENGINE.ui / dp.name[:-6]) for dp in delta_files(ENGINE)]
    eng.append(("ui/menudef.h", MENUDEF))
    pack(ENG_PK3, eng)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["build", "export"])
    ap.add_argument("--export", action="store_true")
    a = ap.parse_args()
    originals = load_originals()
    if a.cmd == "export" or a.export:
        kept = export_layer(ENGINE, originals, line_index(originals))
        print("own files kept whole in git:", ", ".join(kept) or "-")
    if a.cmd == "build":
        generate_layer(ENGINE, originals, export_too=a.export)
        engine_gitignore()
        build_engine_pk3()


if __name__ == "__main__":
    main()
