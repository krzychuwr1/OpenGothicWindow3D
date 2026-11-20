"""Summarize the phase-delimited low-clock logs from a Quest quality sweep."""

import argparse
import csv
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path


PHASES = {
    1: ("75%", 1, 0, 0),
    2: ("100%", 0, 0, 0),
    3: ("125%", 3, 0, 0),
    4: ("150%", 4, 0, 0),
    5: ("75% + CMAA2", 1, 1, 0),
    6: ("75% + enhanced filtering", 1, 0, 1),
}


def logs(path):
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if re.match(r"^\d\d-\d\d ", line):
            stamp = datetime.fromisoformat("2000-" + line[:18])
            yield stamp, line[18:]


def summarize(directory):
    args = (directory / "run.txt").read_text(encoding="utf-8-sig")
    for flag in ("-gpuprofile", "-perf low", "-xrfaceview", "-setting INTERNAL xrSpaceWarp 0",
                 "-setting INTERNAL xrFoveation 0", "-setting INTERNAL xrSurfaceLight 0"):
        if flag not in args:
            raise ValueError(f"missing comparison setting {flag}")
    scene = re.search(r"goto waypoint (\w+)", args).group(1)
    sequence = re.search(r"-setting BENCHMARK xrQualitySweep ([1-6]+)\b", args).group(1)
    game = list(logs(directory / "game.log"))
    runtime = list(logs(directory / "vrapi.log"))
    markers = []
    gpu = []
    for stamp, line in game:
        if m := re.search(r"XR quality ([1-6]) (warmup|begin|end)(.*)", line):
            markers.append((int(m[1]), m[2], stamp, m[3]))
        if m := re.search(r"GPU ms per view ([\d.]+):", line):
            gpu.append((stamp, float(m[1])))
    observed = [(phase, event) for phase, event, _, _ in markers]
    if observed != [(int(c), event) for c in sequence for event in ("warmup", "begin", "end")]:
        raise ValueError("missing, interrupted or unexpected phase sequence")
    for n, code in enumerate(sequence):
        phase = int(code)
        name, res, aa, filt = PHASES[phase]
        warm, begin, end = markers[n*3:n*3+3]
        if f"resIndex={res} aa={aa} filter={filt}" not in warm[3]:
            raise ValueError(f"phase {code} requested an unavailable setting")
        start, finish = begin[2], end[2]
        if (start-warm[2]).total_seconds()<4.9 or (finish-start).total_seconds()<11.9:
            raise ValueError(f"short phase {code}")
        # Both the start and end of each GPU averaging interval must lie inside
        # the measurement phase. VrApi reports the preceding second of display.
        gpu_ms = [value for (prev, _), (stamp, value) in zip(gpu, gpu[1:]) if start<=prev<stamp<=finish]
        fps = []
        for stamp, line in runtime:
            if (stamp-start).total_seconds()<1 or stamp>finish:
                continue
            if not re.search(r"\bCPU4/GPU=1/0,", line):
                raise ValueError(f"phase {code} includes other clocks")
            if m := re.search(r"\bFPS=(\d+)/", line):
                fps.append(int(m[1]))
        if len(gpu_ms)<2 or len(fps)<8:
            raise ValueError(f"phase {code} has only {len(gpu_ms)} GPU intervals and {len(fps)} FPS samples")
        yield (directory.name, scene, name, round(statistics.mean(gpu_ms), 2),
               round(statistics.mean(fps), 2), len(gpu_ms), len(fps))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="+", help="directories containing run.txt, game.log and vrapi.log")
    options = parser.parse_args()
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("run", "scene", "quality", "gpu_ms_per_eye", "fps", "gpu_intervals", "fps_samples"))
    for directory in options.directory:
        try:
            writer.writerows(summarize(directory))
        except (OSError, ValueError, AttributeError) as error:
            parser.exit(1, f"{directory}: {error}\n")
