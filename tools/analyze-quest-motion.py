"""Measure consecutive compositor frames in capture pixels using PyAV, NumPy and SciPy.

Use --preview frame.png to locate a textured, HUD-free rectangle in one eye.
--marker X Y DX DY identifies the first four-bit HUD cell and the displacement
to the next cell; this axis also defines horizontal for a tilted lens image.
Example for a 960x540 Quest physical-display recording:
  python tools/analyze-quest-motion.py display.mp4 --roi 110 173 64 48 --marker 194 90 14 -5 --start 4
"""
import argparse
import csv
from pathlib import Path

import av
import numpy as np
from PIL import Image
from scipy import fft, ndimage


def spectrum(tile, window):
    tile = tile.astype(np.float32)
    features = (tile - ndimage.gaussian_filter(tile, 2)) * window
    return fft.fft2(features)


def register(old, new):
    corr = new * old.conj()
    amp = np.abs(corr)
    if amp.max() == 0:
        return np.nan, np.nan, 0.
    # Weak Fourier bins contain mostly video quantisation noise.
    corr /= np.maximum(amp, amp.max() * 1e-4)
    votes = fft.ifft2(corr).real
    y, x = np.unravel_index(votes.argmax(), votes.shape)
    h, w = votes.shape
    near = votes.copy()
    near[np.ix_((y + np.arange(-3, 4)) % h, (x + np.arange(-3, 4)) % w)] = np.nan
    psr = (votes[y, x] - np.nanmean(near)) / max(np.nanstd(near), 1e-9)
    # Sample the inverse Fourier sum around the peak at 1/16-pixel spacing.
    offsets = np.arange(-12, 13, dtype=np.float32) / 16
    fy, fx = fft.fftfreq(h), fft.fftfreq(w)
    ey = np.exp(2j * np.pi * (y + offsets[:, None]) * fy).astype(np.complex64)
    ex = np.exp(2j * np.pi * (x + offsets[:, None]) * fx).astype(np.complex64)
    fine = (ey @ corr @ ex.T).real
    iy, ix = np.unravel_index(fine.argmax(), fine.shape)
    return (x if x <= w//2 else x-w) + offsets[ix], (y if y <= h//2 else y-h) + offsets[iy], psr


def marker_id(image, marker, guard=False):
    if marker is None:
        return -1
    x, y, dx, dy = marker
    result = 0
    for bit in (range(-1,5) if guard else range(4)):
        a, b = x+bit*dx, y+bit*dy
        if not (1 <= a < image.shape[1]-1 and 1 <= b < image.shape[0]-1):
            raise ValueError('Marker crosses image boundary')
        v = np.median(image[b-1:b+2, a-1:a+2])
        if 68 <= v <= 185:
            return -1
        bright = v>185
        if bit == -1 or bit == 4:
            if bright != (bit == -1):
                return -1
        else:
            result |= int(bright) << bit
    return result


def fiducial_gap(image, box, axis):
    x,y,w,h = box
    if min(w,h)<=0 or x<0 or y<0 or x+w>image.shape[1] or y+h>image.shape[0]:
        raise ValueError('Invalid fiducial ROI')
    r,g,b = image[y:y+h,x:x+w].astype(np.int16).transpose(2,0,1)
    centres = []
    for mask in ((np.minimum(g,b)>110)&(np.minimum(g,b)-r>60),
                 (np.minimum(r,b)>110)&(np.minimum(r,b)-g>60)):
        labels,n = ndimage.label(mask,np.ones((3,3),bool))
        if not n:
            return np.nan
        counts = np.bincount(labels.ravel())[1:]
        largest = np.argmax(counts)
        if counts[largest]<6:
            return np.nan
        yy,xx = np.nonzero(labels==largest+1)
        centres.append(np.array((np.median(xx),np.median(yy))))
    return np.dot(centres[1]-centres[0],axis)


def longest_run(mask):
    padded = np.r_[False, mask, False].astype(int)
    changes = np.diff(padded)
    lengths = np.flatnonzero(changes==-1)-np.flatnonzero(changes==1)
    return int(lengths.max()) if len(lengths) else 0


def marker_brackets(ids, dt, period):
    # A,A,B,B with consecutive IDs contains both views of both application
    # frames if the runtime actually delivered two refreshes per app frame.
    # Restrict to complete pairs with both exterior neighbours observed:
    # exclude long holds, uncertain start/end groups, and long capture gaps.
    keep = np.zeros(len(dt), dtype=bool)
    for i in range(1, len(ids)-4):
        a, b = ids[i], ids[i+2]
        if min(ids[i-1:i+5]) < 0:
            continue
        if (a == ids[i-1] or a != ids[i+1] or b != ids[i+3] or b == ids[i+4]
                or (b-a) % 16 != 1):
            continue
        if np.all((0.8*period <= dt[i:i+3]) & (dt[i:i+3] < 1.5*period)):
            keep[i:i+3] = True
    return keep


def read_warp(path, dtype, channels):
    with path.open('rb') as blob:
        dims = np.fromfile(blob, dtype='<u4', count=2)
        if len(dims) != 2:
            raise ValueError(f'Missing dimensions in {path}')
        w, h = map(int, dims)
        return np.fromfile(blob, dtype=dtype).reshape(h,w,channels)


def compare_inputs(previous, current, roi, csv_path):
    """Compare submitted NDC at current pixels with their observed displacement."""
    with Image.open(previous) as a, Image.open(current) as b:
        old, new = np.asarray(a.convert('L')), np.asarray(b.convert('L'))
    motion = read_warp(current.with_suffix('.mv'), '<f2', 4)
    depth = read_warp(current.with_suffix('.depth'), '<f4', 1)
    old_depth_path = previous.with_suffix('.depth')
    old_depth = read_warp(old_depth_path,'<f4',1) if old_depth_path.exists() else None
    if old.shape != new.shape or depth.shape[:2] != motion.shape[:2]:
        raise ValueError('Colour or warp dimensions changed inside the pair')
    if old_depth is not None and old_depth.shape!=depth.shape:
        raise ValueError('Warp dimensions changed inside the pair')
    h,w = new.shape
    mh,mw = motion.shape[:2]
    # Map active colour subrect coordinates to the active warp subrect. The
    # normalised motion has a range of two across the eye in both dimensions.
    extent = np.array((w,h),dtype=float)
    if roi:
        tiles = [roi]
    else:
        tile = 96
        tiles = [(x,y,tile,tile) for y in range(tile//2,h-tile,tile)
                 for x in range(tile//2,w-tile,tile)]
    rows = []
    for x,y,tw,th in tiles:
        if not (x >= 0 and y >= 0 and min(tw,th) >= 16 and x+tw <= w and y+th <= h):
            raise ValueError('Invalid eye ROI')
        px = np.arange(x+tw//4,x+tw-tw//4)
        py = np.arange(y+th//4,y+th-th//4)
        sample = np.ix_(((py+.5)*mh/h).astype(int),((px+.5)*mw/w).astype(int))
        mv = motion[sample][:,:,:2].reshape(-1,2).astype(np.float32)
        predicted = np.median(mv,axis=0)*extent/2
        if not np.isfinite(predicted).all():
            continue
        # Register RGB independently of the submitted vectors. Drop unresolved
        # peaks near the Fourier wrap, even if they agree with the motion map.
        window = np.outer(np.hanning(th),np.hanning(tw)).astype(np.float32)
        now = new[y:y+th,x:x+tw]
        dx,dy,psr = register(spectrum(old[y:y+th,x:x+tw],window),spectrum(now,window))
        actual = np.array((dx,dy))
        ok = psr>=8 and np.linalg.norm((dx/tw,dy/th))<.30 and np.linalg.norm(actual)>.5
        denom = np.dot(actual,actual)
        gain = np.dot(predicted,actual)/denom if denom>0 else np.nan
        dz = np.nan
        if old_depth is not None and ok:
            yy,xx=np.broadcast_arrays(*sample)
            before=ndimage.map_coordinates(old_depth[:,:,0],[yy-dy*mh/h,xx-dx*mw/w],order=1,mode='nearest')
            dz=np.median(depth[sample][:,:,0]-before)
        rows.append((x,y,*actual,*predicted,psr,float(np.median(depth[sample])),
                     float(now.mean()),gain,np.linalg.norm(predicted-actual),dz,np.median(motion[sample][:,:,2]),bool(ok)))
    labels = ('x','y','rgb_dx_px','rgb_dy_px','mv_dx_px','mv_dy_px','peak_sidelobe',
              'depth_ndc','luma_0_255','gain','error_px','rgb_aligned_depth_delta','mv_z','accepted')
    if csv_path:
        with csv_path.open('w',newline='') as out:
            writer = csv.writer(out)
            writer.writerow(labels)
            writer.writerows(rows)
    print(f'{previous.name} -> {current.name} colour {w}x{h}, warp {mw}x{mh}')
    if roi and rows:
        print(dict(zip(labels,rows[0])))
    reliable = np.asarray([r[:-1] for r in rows if r[-1]])
    if not len(reliable):
        print('No reliable textured, moving tiles')
        return
    print(f'{len(reliable)}/{len(rows)} reliable moving tiles, positive X right, Y down')
    def report(name,selected):
        if len(selected):
            print(f'{name} n={len(selected)} gain p10/50/90 '
                  f'{np.percentile(selected[:,9],[10,50,90]).round(3)}; '
                  f'endpoint error p50/p90 {np.percentile(selected[:,10],[50,90]).round(2)} px')
    report('all',reliable)
    for name,a,b,threshold in (('x (px)',2,4,1.),('y (px)',3,5,.75),('z (NDC)',11,12,.00025)):
        valid = reliable[np.abs(reliable[:,a])>threshold]
        if len(valid):
            actual,vector = valid[:,a],valid[:,b]
            print(f'{name}: n={len(valid)} sign agreement={np.mean(np.sign(actual)==np.sign(vector)):.3f}, '
                  f'median gain={np.median(vector/actual):.4f}, median absolute error={np.median(abs(vector-actual)):.6f}')
    for index,name in ((7,'depth'),(8,'luma')):
        q1,q2 = np.quantile(reliable[:,index],[1/3,2/3])
        for label,sel in ((f'<{q1:.3f}',reliable[:,index]<q1),
                          (f'{q1:.3f}..{q2:.3f}',(reliable[:,index]>=q1)&(reliable[:,index]<=q2)),
                          (f'>{q2:.3f}',reliable[:,index]>q2)):
            report(f'{name} {label}',reliable[sel])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('movie', type=Path,nargs='?')
    parser.add_argument('--warp-pair',type=Path,nargs=2,metavar=('PREVIOUS_PNG','CURRENT_PNG'),
                        help='compare consecutive same-eye PNGs with current .mv and .depth burst sidecars')
    parser.add_argument('--roi', nargs=4, type=int, metavar=('X', 'Y', 'W', 'H'))
    parser.add_argument('--marker', nargs=4, type=int, metavar=('X0', 'Y0', 'DX', 'DY'),
                        help='centre of bit zero and offset between consecutive bit centres')
    parser.add_argument('--guard-marker', action='store_true',
                        help='require a white cell before bit 0 and a black cell after bit 3 (new captures)')
    parser.add_argument('--fiducial', nargs=4, type=int, metavar=('X','Y','W','H'),
                        help='box covering just the trial-10 cyan and magenta bars in the same eye')
    parser.add_argument('--hz', type=float, default=72)
    parser.add_argument('--csv', type=Path)
    parser.add_argument('--preview', type=Path)
    parser.add_argument('--start', type=float, default=0.)
    parser.add_argument('--end', type=float, default=float('inf'))
    args = parser.parse_args()
    if args.warp_pair:
        if args.movie:
            parser.error('Choose a movie or a pair of eye images')
        return compare_inputs(*args.warp_pair,args.roi,args.csv)
    if args.movie is None:
        parser.error('Give a display movie or --warp-pair')
    if args.roi is None and args.preview is None:
        parser.error('Give --preview to locate the window or --roi to measure it')
    if (args.fiducial or args.guard_marker) and not args.marker:
        parser.error('--fiducial and --guard-marker require --marker')
    if args.marker:
        axis = np.array(args.marker[2:], dtype=float)
        if np.linalg.norm(axis)==0:
            parser.error('Marker cells must have a nonzero separation')
        axis /= np.linalg.norm(axis)
    rows, prev_ts, prev_marker = [], None, None
    gaps = []
    origin = None
    with av.open(str(args.movie)) as movie:
        stream = movie.streams.video[0]
        print(f'{stream.width}x{stream.height} reported fps={stream.average_rate}, time base={stream.time_base}')
        for frame in movie.decode(stream):
            if frame.pts is None:
                raise ValueError('Missing presentation timestamp')
            ts = float(frame.pts * frame.time_base)
            if origin is None:
                origin = ts
            ts -= origin
            if ts < args.start:
                continue
            if ts > args.end:
                break
            if args.preview:
                frame.to_image().save(args.preview)
                args.preview = None
                if args.roi is None:
                    return
            image = frame.to_ndarray(format='gray')
            x, y, w, h = args.roi
            if not (x >= 0 and y >= 0 and min(w,h) >= 16 and x+w <= image.shape[1] and y+h <= image.shape[0]):
                raise ValueError('Invalid ROI')
            tile = image[y:y+h, x:x+w]
            if prev_ts is None:
                window = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
            current = spectrum(tile, window)
            marker = marker_id(image, args.marker,args.guard_marker)
            if args.fiducial:
                gaps.append(fiducial_gap(frame.to_ndarray(format='rgb24'),args.fiducial,axis))
            if prev_ts is not None:
                dx, dy, psr = register(previous, current)
                mad = np.mean(np.abs(tile.astype(np.int16)-previous_tile))
                rows.append((ts, 1000*(ts-prev_ts), dx, dy, psr, mad, prev_marker, marker))
            previous, previous_tile, prev_ts, prev_marker = current, tile.copy(), ts, marker
    if not rows:
        raise ValueError('No frame pairs in interval')
    if args.csv:
        with args.csv.open('w', newline='') as out:
            writer = csv.writer(out)
            header = ('t_sec', 'dt_ms', 'dx_right_px', 'dy_down_px', 'peak_sidelobe', 'mean_abs_delta', 'previous_id', 'id')
            writer.writerow(header + (('previous_bar_gap_px','bar_gap_px') if gaps else ()))
            writer.writerows(row+(gaps[i],gaps[i+1]) if gaps else row for i,row in enumerate(rows))
    t, dt, dx, dy, psr, mad, mid0, mid1 = np.array(rows).T
    target = 1000/args.hz
    regular = np.abs(dt-target) <= target * 0.15
    # A near-refresh interval alone is insufficient: an asynchronous ~60 Hz
    # encoder timestamp can drift through every phase of a 72 Hz display.
    ticks = np.r_[t[0]-dt[0]/1000,t]*args.hz
    phasor = np.exp(2j*np.pi*ticks).mean()
    phase = np.angle(phasor)/(2*np.pi)
    indices = np.rint(ticks-phase).astype(int)
    close = np.abs(ticks-phase-indices) < 0.18
    locked = (abs(phasor)>=0.75) & close[:-1] & close[1:] & (np.diff(indices)==1)
    good = (psr >= 8) & np.isfinite(dx)
    print(f'{len(rows)} pairs in {dt.sum()/1000:.3f} s, {1000/dt.mean():.2f} fps; '
          f'{regular.sum()}/{len(dt)} intervals within 15% of one {args.hz:g} Hz refresh; '
          f'PTS dt p10/50/90 {np.percentile(dt,[10,50,90]).round(2)} ms')
    expected = int(round(dt.sum()/target))
    print(f'{expected} nominal refresh intervals over clip; excess over captured intervals {expected-len(dt)}. '
          f'{args.hz:g}-Hz phase coherence={abs(phasor):.3f} (threshold 0.75); '
          f'{locked.sum()} PTS-lattice pairs, longest contiguous run {longest_run(locked)}')
    print(f'{good.sum()} reliable correlation peaks (PSR >= 8); {(mad == 0).sum()} identical decoded ROIs')
    print('First signed horizontal shifts (px, * = low confidence):')
    print(', '.join(f'{v:+.2f}' + ('' if ok else '*') for v,ok in zip(dx[:32],good[:32])))
    if good.any():
        v = dx[good]
        print(f'Shift median={np.median(v):+.3f} px; p10/p90={np.percentile(v,[10,90]).round(3)} px')
    if args.marker:
        known = (mid0 >= 0) & (mid1 >= 0)
        ids = np.r_[mid0[0],mid1]
        brackets = marker_brackets(ids, dt, target)
        print(f'{brackets.sum()} capture pairs bracketed by consecutive two-picture app IDs; '
              f'longest contiguous run {longest_run(brackets)}. Check VrApi for 2:1 cadence.')
        print(f'Unknown marker pairs: {(~known).sum()}; visible app ID gaps: '
              f'{(known & ((mid1-mid0)%16 > 1)).sum()}')
        longitudinal = dx*axis[0] + dy*axis[1]
        # Pair both increments for each A,A,B triplet rather than mixing
        # different locations in a moving/deforming scene.
        pair = (brackets[:-1] & brackets[1:] & good[:-1] & good[1:] &
                (ids[:-2] == ids[1:-1]) & ((ids[2:]-ids[1:-1]) % 16 == 1))
        old, new = longitudinal[:-1][pair], longitudinal[1:][pair]
        if len(old):
            print(f'{len(old)} complete A,A,B triplets; along window horizontal '
                  f'unchanged ID p10/50/90 {np.percentile(old,[10,50,90]).round(3)} px; '
                  f'new ID {np.percentile(new,[10,50,90]).round(3)} px')
        strong = np.abs(old+new) > 0.4
        if np.any(strong):
            fraction = old[strong]/(old[strong]+new[strong])
            print(f'Fraction of each game-frame displacement on the intervening capture '
                  f'(n={len(fraction)}, ideal 0.5): p10/50/90 '
                  f'{np.percentile(fraction,[10,50,90]).round(3)}, '
                  f'RMS error {np.sqrt(np.mean((fraction-0.5)**2)):.3f}')
        else:
            print('No resolved 2:1 game-frame steps in the selected interval.')
        if gaps:
            gaps = np.asarray(gaps)
            finite = gaps[np.isfinite(gaps)]
            if not len(finite):
                print('Could not locate both calibration bars.')
                return
            low,high = np.percentile(finite,[20,90])
            if high-low<1.5:
                print(f'No resolved calibration-bar split: p20/p90 gap {low:.2f}/{high:.2f} px')
                return
            base = abs(gaps-low)<.8
            shifted = gaps>low+max(1.5,(high-low)*.30)
            marked = pair & base[:-2] & shifted[1:-1] & base[2:]
            total = longitudinal[:-1]+longitudinal[1:]
            marked &= abs(total)>.4
            print(f'Trial-10 cyan-to-magenta gap p20/p90 {low:.2f}/{high:.2f} px; '
                  f'{np.sum(shifted)} visibly shifted, {np.sum(base)} baseline, {len(gaps)-np.sum(shifted|base)} ambiguous.')
            if marked.any():
                f=longitudinal[:-1][marked]/total[marked]
                print(f'{marked.sum()} complete A,A,B triplets with baseline/shifted/baseline bars: '
                      f'scene fraction p10/50/90 {np.percentile(f,[10,50,90]).round(3)}, RMS error {np.sqrt(np.mean((f-.5)**2)):.3f}')
            else:
                print('No complete bracket with an independently visible calibration shift.')


if __name__ == '__main__':
    main()
