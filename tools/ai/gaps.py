"""A work list of what the game asks for that the runtime does not implement, from the `gaps.tsv` files runs leave next to their log.

Every place in the runtime that answers with a stand-in (an NVN entry point without an implementation, a draw skipped or drawn without an
untranslated shader stage, an operation the Vulkan back end leaves out, an unhandled IPC command, an unimplemented import, an unknown SVC)
counts it with `nxrt::gap()` (runtime/include/nxrt/gaps.h). This script merges the files of one or more runs, joins shader hashes with the
shader cache's `results.tsv` (stage, translator status and error), groups related rows and prints them ranked, with the artifact row of
AGENTS.md where one is known. It needs no screenshots: a scene that "looks fine" can still be missing a whole class of geometry.

usage:
  gaps.py <run dir or gaps.tsv> [...] [--cache <shader cache dir>] [--top N] [--all] [--md FILE]
  gaps.py work\\run\\gaps_1 --cache ..\\..\\work\\shaders\\oot\\spirv

Columns: count (sum over the runs), runs (in how many of the given runs it appeared), area, what, detail. Draw/dispatch counts are per
draw call, so they say how much of the picture is affected; nvn-proc counts are calls.
"""
import argparse
import collections
import csv
import os
import re
import sys

# Known gaps -> the row of the artifact table in AGENTS.md (pattern on "area what detail", first match wins). Extend as rows are added.
KNOWN = [
    (r"DrawMeshTasks|stage 6|stage 7|mesh|task", "A9/A15 mesh shaders (foliage, Deku Tree)"),
    (r"WaitEvent", "GPU-side WaitEvent (nvn)"),
    (r"TEX\.SCR\.B\.DC|shadow", "A7 clustered-light shadows"),
]
def default_cache():
    """NXRT_NVN_SHADER_CACHE, else work/shaders/oot/spirv of this checkout, else of the main checkout (a worktree has no work/)."""
    if os.environ.get("NXRT_NVN_SHADER_CACHE"):
        return os.environ["NXRT_NVN_SHADER_CACHE"]
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    here = os.path.join(repo, "work", "shaders", "oot", "spirv")
    if os.path.exists(os.path.join(here, "results.tsv")):
        return here
    m = re.search(r"^(.*?)[\\/]\.claude[\\/]worktrees[\\/]", repo)
    return os.path.join(m.group(1), "work", "shaders", "oot", "spirv") if m else here


DEFAULT_CACHE = default_cache()
HASH = re.compile(r"\b[0-9a-f]{16}\b")


def load_gaps(paths):
    rows = collections.defaultdict(lambda: [0, 0, 1e18, 0.0])   # key -> count, runs, first_s, last_s
    used = 0
    for p in paths:
        f = os.path.join(p, "gaps.tsv") if os.path.isdir(p) else p
        if not os.path.exists(f):
            print(f"(no gaps.tsv in {p})", file=sys.stderr)
            continue
        used += 1
        with open(f, encoding="utf-8", errors="replace", newline="") as fh:
            for r in csv.DictReader(fh, delimiter="\t"):
                k = (r["area"], r["what"], r["detail"])
                e = rows[k]
                e[0] += int(r["count"])
                e[1] += 1
                e[2] = min(e[2], float(r["first_s"]))
                e[3] = max(e[3], float(r["last_s"]))
    return rows, used


def load_results(cache):
    res = {}
    f = os.path.join(cache, "results.tsv")
    if not os.path.exists(f):
        return res
    with open(f, encoding="utf-8", errors="replace", newline="") as fh:
        rd = csv.reader(fh, delimiter="\t")
        head = next(rd)
        ix = {n: i for i, n in enumerate(head)}
        for r in rd:
            if len(r) < len(head):
                continue
            res[r[0]] = (r[ix["stage"]], r[ix["status"]], r[ix["error"]][:160])
    return res


def shader_note(detail, results):
    notes = []
    for h in HASH.findall(detail):
        st = results.get(h)
        notes.append(f"{h}: {st[0]} {st[1]} ({st[2]})" if st else f"{h}: no results.tsv row (never translated: on-demand or new)")
    return "; ".join(notes)


def known(text):
    for pat, row in KNOWN:
        if re.search(pat, text, re.I):
            return row
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--cache", default=DEFAULT_CACHE, help="shader cache with results.tsv (default: the main checkout's)")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--all", action="store_true", help="every row (default: the --top most frequent)")
    ap.add_argument("--md", help="also write the list as a Markdown table to this file")
    a = ap.parse_args()
    rows, used = load_gaps(a.runs)
    if not used:
        sys.exit("no gaps.tsv found")
    results = load_results(a.cache)
    # Rows about shader blocks are grouped by the translator's error class (normalized), so 140 mesh blocks with the same reason are one row;
    # the group lists its most frequent blocks. Other rows stay as they are.
    groups = {}
    for (area, what, detail), (count, runs, first_s, last_s) in rows.items():
        note = shader_note(detail, results) if area in ("draw", "dispatch", "shader") else ""
        if note:
            st = [results.get(h) for h in HASH.findall(detail)]
            cls = "; ".join(f"{x[0]} {x[1]}: {re.sub(r'0x[0-9a-f]+|[0-9]+', 'N', x[2])[:110]}" if x else "no results.tsv row" for x in st)
            key = (area, what, cls)
        else:
            key = (area, what, detail)
        g = groups.setdefault(key, {"count": 0, "runs": 0, "items": []})
        g["count"] += count
        g["runs"] = max(g["runs"], runs)
        g["items"].append((count, detail, note))
    lines = []
    for (area, what, d), g in groups.items():
        items = sorted(g["items"], key=lambda x: -x[0])
        if len(items) == 1:
            detail, note = items[0][1], items[0][2]
        else:
            detail = f"{len(items)} blocks, {d}"
            note = "most frequent: " + ", ".join(f"{x[1]} ({x[0]})" for x in items[:4])
        tag = known(" ".join((area, what, detail, note)))
        lines.append((g["count"], g["runs"], area, what, detail, note, tag, 0.0))
    lines.sort(key=lambda x: (-x[0], x[2], x[3], x[4]))
    by_area = collections.Counter()
    for ln in lines:
        by_area[ln[2]] += ln[0]
    print(f"{used} run(s), {len(rows)} distinct gaps in {len(lines)} rows; totals by area: " + ", ".join(f"{k} {v}" for k, v in by_area.most_common()))
    print()
    shown = lines if a.all else lines[:a.top]
    for count, runs, area, what, detail, note, tag, first_s in shown:
        print(f"{count:>10} {runs:>2}x {area:<9} {what}{': ' + detail if detail else ''}" + (f"   [{tag}]" if tag else ""))
        if note:
            print(f"{'':>24}{note}")
    if len(shown) < len(lines):
        print(f"... {len(lines) - len(shown)} more (--all)")
    # Untranslated shader blocks by error class, weighted by the draws that needed them.
    cls = collections.Counter()
    for (area, what, detail), (count, runs, first_s, last_s) in rows.items():
        if area not in ("draw", "dispatch"):
            continue
        for h in HASH.findall(detail):
            st = results.get(h)
            err = re.sub(r"0x[0-9a-f]+|\d+", "N", st[2])[:90] if st else "no results.tsv row"
            cls[(st[0] if st else "?", st[1] if st else "?", err)] += count
    if cls:
        print("\nuntranslated shader stages by translator error, weighted by draws/dispatches that needed them:")
        for (stage, status, err), n in cls.most_common(25):
            print(f"{n:>10}  {stage:<9} {status:<12} {err}")
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write("| count | runs | area | what | detail | known as |\n|---:|---:|---|---|---|---|\n")
            for count, runs, area, what, detail, note, tag, first_s in lines:
                d = (detail + (" — " + note if note else "")).replace("|", "/")
                f.write(f"| {count} | {runs} | {area} | {what} | {d} | {tag} |\n")
        print(f"\nwrote {a.md}")


if __name__ == "__main__":
    main()
