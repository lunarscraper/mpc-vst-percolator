#!/usr/bin/env python3
"""Patch the shared wrapper (mpc-vst-plugins wrapper/vst2_wrap.c) before the build: long option lists step one option
per Q-Link event or data-wheel click.

Why: with QLINK_TRAVEL the wrapper keeps the host's unrounded position ("shadow") and adds every event to it. That is
right for a short list (a Q-Link event is 1/128 of the range, a fraction of an option of a 3-way switch), but on the
128-entry NOTE lists one data-wheel click is 0.01 of the range = 1.27 notes: the remainders add up and every fourth
click jumps two notes (D1 -> D#1 -> F1: E1 can't be reached with the wheel, 2026-11-08 on the Force).

What changes: when one host event moves half an option or more (only long lists: 128 notes; a 3-way switch moves 1/43
of an option per event), the wrapper steps from the option the engine holds by the event's size in whole options
(at least one) and keeps no shadow. Short lists, presets, automation and picks (values on an option) work as before.

Usage (release.yml "build:"): python3 mpc/wrap_fix.py "$MPC_VST/wrapper/vst2_wrap.c" && bash .../build_port.sh ...
The script patches in place, refuses a wrapper it doesn't recognise, and does nothing if it is already patched.
"""
import sys

OLD = """    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f) {
            nudge = 1;
            shadow = clamp01(n);
            n = roundf(pos) / (p->nopts - 1);
        }
    }"""

NEW = """    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f) {
            nudge = 1;
            /* wrap_fix (Percolator): one host event of half an option or more (a long list such as 128 notes) steps
             * from the option the engine holds, by the event's size in whole options, without a shadow */
            float rb = get_norm(w, i) * (p->nopts - 1), dlt = pos - rb;
            char wf_cur[64];
            if (fabsf(dlt) >= 0.5f && eng_get(w, p->key, wf_cur, sizeof wf_cur) > 0) {
                float base = roundf(str_to_norm(p, wf_cur) * (p->nopts - 1)), k = fmaxf(1.0f, roundf(fabsf(dlt)));
                float idx = base + (dlt > 0 ? k : -k);
                if (idx < 0) idx = 0;
                if (idx > p->nopts - 1) idx = (float)(p->nopts - 1);
                n = idx / (p->nopts - 1);
            } else {
                shadow = clamp01(n);
                n = roundf(pos) / (p->nopts - 1);
            }
        }
    }"""

MARK = "wrap_fix (Percolator)"


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: wrap_fix.py path/to/vst2_wrap.c")
    path = sys.argv[1]
    src = open(path, encoding="utf-8").read()
    if MARK in src:
        print("wrap_fix: already patched")
        return
    if src.count(OLD) != 1:
        sys.exit("wrap_fix: the QLINK_TRAVEL option block in %s is not the one this fix knows (wrapper changed?)" % path)
    open(path, "w", encoding="utf-8").write(src.replace(OLD, NEW))
    print("wrap_fix: patched %s" % path)


if __name__ == "__main__":
    main()
