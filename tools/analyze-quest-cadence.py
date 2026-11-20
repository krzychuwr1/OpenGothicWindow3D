"""Join a Quest screenrecord, its VrApi logcat and an analyze-quest-motion.py --csv file.

Pass the headset's UTC offset at capture time and the marker's DX,DY as --axis.
A repeat changes sampled whole-image luma by less than 0.35 levels on a 0–255
scale. A region holds when it moves less than 0.3 pixels along the given axis.
The displacement summary uses complete frame-ID triplets in full-rate AppSW
seconds (within two of refresh rate) with zero tears and stale frames. VrApi
reports the preceding second in aggregate and does not identify each scanout.
"""
import argparse
import importlib
import mmap
import re
import struct
from datetime import datetime, timezone
from pathlib import Path

import av
import numpy as np


def decode(movie):
    # Android frameworks/av cmds/screenrecord/screenrecord.cpp, writeWinscopeMetadata.
    magic = b'#VV1NSC0PET1ME2#'
    with movie.open('rb') as src, mmap.mmap(src.fileno(), 0, access=mmap.ACCESS_READ) as buf:
        pos = buf.find(magic)
        if pos < 0:
            raise ValueError('Recording has no Android Winscope v2 timestamps')
        pos += len(magic)
        version, offset, count = struct.unpack_from('<IqI', buf, pos)
        if version != 2 or count < 2 or pos + 16 + count * 8 > len(buf):
            raise ValueError('Invalid Winscope timing record')
        wall = (np.frombuffer(buf, '<i8', count, pos+16) + offset) * 1e-9
    pts, changes = [], [np.nan]
    previous = None
    with av.open(str(movie)) as video:
        for frame in video.decode(video=0):
            pts.append(frame.time)
            picture = frame.to_ndarray(format='gray')[::4, ::4].astype(np.int16)
            if previous is not None:
                changes.append(np.abs(picture-previous).mean())
            previous = picture
    if len(wall) != len(pts) or np.any(np.diff(wall) <= 0):
        raise ValueError('Winscope timestamps and decoded frames disagree')
    pts = np.asarray(pts) - pts[0]
    error = np.max(np.abs((wall-wall[0])-pts))
    if error > .001:
        raise ValueError(f'Winscope and video PTS drift by {1000*error:.2f} ms')
    print(f'{len(pts)} encoded images, first at {datetime.fromtimestamp(wall[0], timezone.utc).isoformat()}, '
          f'maximum relative PTS difference {1000*error:.3f} ms')
    return pts, wall, np.asarray(changes)


def reports(path, zone, first, last):
    # Threadtime logcat lacks the year and zone. Match only the recording's dates.
    dates = {datetime.fromtimestamp(t, zone).strftime('%m-%d'): datetime.fromtimestamp(t, zone).year
             for t in (first-1, last+1)}
    latest = {}
    found = []
    for line in path.read_text(errors='replace').splitlines():
        match = re.match(r'(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(\d+)\s+\d+\s+\w\s+VrApi\s*:\s*(.*)', line)
        if not match:
            continue
        stamp, pid, values = match.groups()
        if stamp[:5] not in dates:
            continue
        t = datetime.fromisoformat(f'{dates[stamp[:5]]}-{stamp}').replace(tzinfo=zone).timestamp()
        if t < first-1 or t > last+1:
            continue
        fields = dict(re.findall(r'(\w+)=([^,\s]+)', values))
        if 'FPS' in fields:
            latest[pid] = (t, fields)
            found.append((t, pid, fields))
        elif 'ASW' in fields and pid in latest and abs(t-latest[pid][0]) < .1:
            latest[pid][1].update(fields)
    if len({pid for _, pid, _ in found}) != 1:
        raise ValueError('Expected one overlapping VrApi process. Check logcat format, UTC offset and game PID.')
    return found


def analyze(args):
    pts, wall, delta = decode(args.movie)
    zone = datetime.fromisoformat('2000-01-01T00:00:00'+args.utc_offset).tzinfo
    if zone is None:
        raise ValueError('Supply a UTC offset such as +01:00')
    logs = reports(args.log, zone, wall[0], wall[-1])
    r = np.atleast_1d(np.genfromtxt(args.motion_csv, delimiter=',', names=True))
    if len(r) < 4:
        raise ValueError('Need at least four consecutive motion CSV rows')
    # Each CSV row describes the transition ending at its t_sec.
    right = np.clip(pts.searchsorted(r['t_sec']), 1, len(pts)-1)
    end = np.where(np.abs(pts[right]-r['t_sec']) < np.abs(pts[right-1]-r['t_sec']), right, right-1)
    if (end[0] < 1 or np.any(np.diff(end) != 1) or np.max(np.abs(pts[end]-r['t_sec'])) > .001
            or np.max(np.abs(pts[end]-pts[end-1]-r['dt_ms']*.001)) > .001
            or np.any(r['previous_id'][1:] != r['id'][:-1])):
        raise ValueError('Motion CSV does not match consecutive timestamps and IDs in this recording')
    ids = np.r_[r['previous_id'][0], r['id']].astype(int)
    axis = np.asarray(args.axis)
    if not np.isfinite(axis).all() or np.linalg.norm(axis) == 0:
        raise ValueError('Give the nonzero image-space direction of the window horizontal')
    shift = (r['dx_right_px']*axis[0] + r['dy_down_px']*axis[1])/np.linalg.norm(axis)
    good = (ids[:-1] >= 0) & (ids[1:] >= 0) & (r['peak_sidelobe'] >= 12)
    duplicate = delta[end] < .35
    brackets = importlib.import_module('analyze-quest-motion').marker_brackets
    accepted, changing = [], []
    print('report end        frames/repeats   ROI holds/good    FPS    ASW   Stale  Tear  E(mean/RMS)')
    seconds = 0
    for t, _, stat in logs:
        if t-1 < wall[end[0]-1] or t > wall[end[-1]]:
            continue
        period = 1/float(stat['FPS'].split('/')[1])
        dt = r['dt_ms']*.001
        regular = good & (dt > .8*period) & (dt < 1.5*period)
        window = (wall[end] > t-1) & (wall[end] <= t)
        valid = window & regular
        print(f'{datetime.fromtimestamp(t, zone):%H:%M:%S.%f}  {window.sum():3}/{duplicate[window].sum():2}             '
              f'{(np.abs(shift[valid]) < .3).sum():2}/{valid.sum():2}        {stat["FPS"]:5}  '
              f'{stat.get("ASW","-"):3}   {stat["Stale"]:>3}   {stat["Tear"]:>3}  {stat.get("E","-")}')
        seconds += 1
        if (stat.get('Type') != 'App' or int(stat['Stale']) or int(stat['Tear'])
                or int(stat.get('ASW', 0)) < round(1/period)-2):
            continue
        b = brackets(ids.tolist(), dt, period)
        pair = (b[:-1] & b[1:] & valid[:-1] & valid[1:] & (wall[end[:-1]-1] >= t-1)
                & (ids[:-2] == ids[1:-1]) & ((ids[2:]-ids[1:-1]) % 16 == 1))
        total = shift[:-1]+shift[1:]
        pair &= np.abs(total) > .4
        accepted.extend(shift[:-1][pair]/total[pair])
        pair &= ~duplicate[:-1] & ~duplicate[1:]
        changing.extend(shift[:-1][pair]/total[pair])
    if not seconds:
        raise ValueError('No complete VrApi report seconds within the motion CSV')
    for name, sample in [('All', accepted), ('Both encoded transitions changed', changing)]:
        print(f'{name}: {len(sample)} accepted AppSW triplets' +
              (f', intervening displacement fraction p25/50/75 {np.round(np.percentile(sample,[25,50,75]),3)}'
               if sample else ''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path)
    parser.add_argument('log', type=Path, help='threadtime VrApi logcat for this recording')
    parser.add_argument('motion_csv', type=Path, help='analyze-quest-motion.py --csv with a decoded frame-ID marker')
    parser.add_argument('--utc-offset', required=True, help='headset logcat UTC offset, e.g. +01:00')
    parser.add_argument('--axis', nargs=2, type=float, required=True, metavar=('DX','DY'))
    try:
        analyze(parser.parse_args())
    except (ValueError, KeyError, OSError, struct.error) as exc:
        parser.exit(2, f'Cadence analysis failed: {exc}\n')
