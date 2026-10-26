#!/usr/bin/env python3
"""ROM-backed margin activation: real AI and projectiles, no enemy edits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import shutil

from sm_progression_qa import Session, Rom


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--artifacts', type=Path, required=True)
    p.add_argument('--aspect', default='16:9', choices=('4:3', '16:9', '21:9', '32:9'))
    p.add_argument('--expect-native', action='store_true', help='reference build without wider activity')
    p.add_argument('--fixture', type=Path, help='same-build AFCE right-margin snapshot; bypasses boot/traversal')
    args = p.parse_args()
    original = subprocess.Popen

    def launch(*a, **kw):
        video = Path(kw['cwd'])/'sm-video.ini'
        video.write_text('[SuperMetroidVideo]\nEnhancedRenderer=1\nAspect='+args.aspect+'\nPresentationEnabled=0\n')
        return original(*a, **kw)

    subprocess.Popen = launch
    s = Session(args.exe, args.rom, args.artifacts, wide=True, capture_profile=False)
    report = {'passed': False, 'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(), 'aspect': args.aspect}
    try:
        rom = Rom(args.rom)
        rooms = rom.rooms()
        if args.fixture:
            shutil.copyfile(args.fixture, args.artifacts/'saves/save3.sav')
            s.step(2); s.command('loadstate 3'); s.step(2)
            assert s.state()['room'] == 0xafce and s.state()['state'] == 8
            report['fixture'] = str(args.fixture.resolve())
        else:
            s.boot(); s.land()
            for at, value in ((0x9c2, 1999), (0x9c4, 1999)):
                s.word(at, value)
            door = next(d for d in rooms[0xa923]['doors'] if d['destination'] == 0xafce)
            report['entry'] = s.transition(door)
        # Ordinary movement drives scrolling. Hover only bypasses lava/floor
        # gaps; never change enemies, AI counters, projectiles or success bits.
        def hover(frames, buttons='0'):
            for _ in range(frames):
                for at, value in ((0xafa, 96), (0xb14, 96), (0xb2c, 0), (0xb2e, 0), (0x18a8, 120)):
                    s.word(at, value)
                s.step(1, buttons)
        enemy = 0xf78 + 7*64
        assert s.word(enemy) == 0xd4bf
        def observation():
            return dict(frame=s.command('frame')['frame'], camera=s.word(0x911),
                        x=s.word(enemy+2)-s.word(0x911), y=s.word(enemy+6),
                        counter=s.word(enemy+44), properties=s.word(enemy+14))
        for _ in range(700):
            if observation()['x'] <= 275: break
            hover(1, 'right')
        else: raise AssertionError('could not reach right margin')
        hover(2)
        before = observation()
        assert 265 <= before['x'] <= 280, before
        s.command('savestate 3')
        projectiles = []
        for _ in range(240):
            hover(1)
            projectiles_ram = s.read(0x1997, 0x120)
            camera = s.word(0x911)
            for i in range(18):
                identity = struct.unpack_from('<H', projectiles_ram, 2*i)[0]
                if identity:
                    x = struct.unpack_from('<H', projectiles_ram, 0xb4+2*i)[0]-camera
                    if x >= 256 or x < 0:
                        projectiles.append(dict(id=identity, x=x, y=struct.unpack_from('<H', projectiles_ram, 0xfc+2*i)[0]))
        after = observation()
        active = args.aspect != '4:3' and not args.expect_native
        assert ((after['counter']-before['counter']) & 65535) == (240 if active else 0), (before, after)
        if active: assert projectiles, 'active margin enemy never emitted a visible margin projectile'
        report.update(before=before, after=after, margin_projectiles=projectiles[:20])
        s.command('loadstate 3'); hover(2)
        loaded = observation(); hover(60); resumed = observation()
        assert ((resumed['counter']-loaded['counter']) & 65535) == (60 if active else 0)
        report.update(loaded=loaded, resumed=resumed)
        # Controlled laser fixtures execute the real ROM pre-instructions.
        # They bypass enemy spawning; this specifically tests culling, not
        # natural pirate combat. Real seahorse spawning was checked above.
        laser = 0xa17b
        assert rom.word(0x860000 | laser) == 0xa009
        culls = []
        extra = {'4:3': 0, '16:9': 43, '21:9': 96, '32:9': 213}[args.aspect]
        for side, start, pre in [('left', -20, 0xa05c), ('right', 275, 0xa07a)]:
            for at, value in ((0x1997, laser), (0x1a03, pre), (0x1a4b, s.word(0x911)+start),
                              (0x1a93, s.word(0x915)+96), (0x1aff, 0), (0x1b8f, 32767),
                              (0x1b6b, 0x8000), (0x1bd7, 0x2000)):
                s.word(at, value)
            hover(4)
            kept = s.word(0x1997) == laser
            assert kept == active, (side, kept, active)
            culls.append(dict(side=side, kept_after_four_frames=kept))
            hover(60)
            assert s.word(0x1997) != laser, 'laser survives outside the extended view'
        report['controlled_laser_culls'] = culls
        # A real uncharged beam must survive the old +/-64 padding in 32:9.
        hover(1, 'right'); hover(1, 'x')
        beam_x = []
        for _ in range(60):
            hover(1)
            for i in range(5):
                if s.word(0xc40+2*i): beam_x.append(s.word(0xb64+2*i)-s.word(0x911))
        report['real_beam_x'] = beam_x
        if active and args.aspect == '32:9':
            assert max(beam_x, default=0) > 320, 'beam disappeared at the old culling boundary'
        s.command('screenshot margin.bmp')
        report['passed'] = True
    finally:
        s.close(); subprocess.Popen = original
        (args.artifacts/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS:', args.aspect, 'margin AI '+('active' if active else 'native culling')+', real projectiles and save/load')


if __name__ == '__main__': main()
