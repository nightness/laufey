#!/usr/bin/env python3
"""Print the useful part of macOS crash reports (.ips): the exception, the
crashed thread's backtrace and the main thread's, with image names and
symbols. A whole report runs to hundreds of KB (CEF has ~50 threads), more
than a CI log should carry.

  scripts/print-crash-report.py <report.ips>...
"""

import json
import sys


def frames(report, thread, limit=60):
    images = report.get("usedImages", [])
    out = []
    for f in thread.get("frames", [])[:limit]:
        idx = f.get("imageIndex")
        image = images[idx] if idx is not None and idx < len(images) else {}
        name = image.get("name") or image.get("path") or "???"
        sym = f.get("symbol")
        where = (
            f"{sym} + {f.get('symbolLocation', 0)}"
            if sym
            else hex(f.get("imageOffset", 0))
        )
        src = f.get("sourceFile")
        if src:
            where += f" ({src}:{f.get('sourceLine', '?')})"
        out.append(f"    {name:<36} {where}")
    return out


def show(path):
    print(f"== {path}")
    with open(path, encoding="utf-8", errors="replace") as fh:
        header = fh.readline()
        body = fh.read()
    try:
        report = json.loads(body)
    except ValueError:
        # Not the two-part JSON format: print the start of it as text.
        print(header + body[:20000])
        return
    print("   ", header.strip()[:300])
    print("    exception:", json.dumps(report.get("exception")))
    term = report.get("termination")
    if term:
        print("    termination:", json.dumps(term)[:500])
    asi = report.get("asi")
    if asi:
        print("    asi:", json.dumps(asi)[:1000])
    threads = report.get("threads", [])
    crashed = report.get("faultingThread")
    for i, t in enumerate(threads):
        if i == crashed or i == 0:
            label = "crashed" if i == crashed else "main"
            print(
                f"  thread {i} ({label}) {t.get('name', '')} "
                f"{t.get('queue', '')}".rstrip()
            )
            print("\n".join(frames(report, t)))


for p in sys.argv[1:]:
    try:
        show(p)
    except Exception as e:  # never fail the CI step over a report
        print(f"   (could not parse {p}: {e})")
