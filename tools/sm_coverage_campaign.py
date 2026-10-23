#!/usr/bin/env python3
"""Run the reproducible Super Metroid coverage campaign against one executable.

Every stage is a headless tools/sm_progression_qa.py session. The first stages
start from power-on; later stages seed from host savestates written earlier in
the SAME campaign, i.e. by the same executable, so no snapshot ever crosses a
build. Each session captures tier-2 coverage from instruction zero.

    run       execute the stages, then bundle and summarize
    bundle    re-bundle/summarize an existing campaign directory
    compare   compare two summarized campaigns (e.g. before/after, or a build
              against its interpreter-only reference)

The bundle (`snesrecomp tier2 bundle v2`) contains each session's final
complete checkpoint, falling back to its journal only when the checkpoint is
missing. All records share one build identity, so the generator may read the
bundle as a single historical profile. Observations seed analysis only; the
generator still decides what is AOT-eligible, and every promotion must be
qualified by rerunning this campaign on the rebuilt executable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
QA = ROOT / 'tools' / 'sm_progression_qa.py'
BOSS_ROOMS = ('b32e', 'cd13', 'a59f')
REMAINING_EQUIPMENT = ('ac2b', 'ce40', 'a107')
CERES_ROOMS = ('df45', 'df8d', 'dfd7', 'e021', 'e06b', 'e0b5')

# name -> (seed, arguments). A seed is (stage, file) inside this campaign.
STAGES = {
    'attract': (None, ['--attract', '10800']),
    'equipment': (None, ['--landing-probe', '--equipment-route', '--station-probe',
                         '--invincible', '--assist-pickups', '--save-each-room',
                         '--save-probe']),
    'equipment-remaining': (('equipment', 'room-b6c1.sav'),
                            ['--rooms', *REMAINING_EQUIPMENT, '--collect-items', '--direct',
                             '--invincible', '--assist-pickups', '--post-boss',
                             '--save-each-room', '--save-probe']),
    'sweep-pre': (('equipment', 'room-b6c1.sav'), ['--sweep', '--invincible']),
    # The sweep covers Zebes (areas 0-5); Ceres is area 6.
    'ceres': (('equipment', 'room-b6c1.sav'),
              ['--rooms', *CERES_ROOMS, '--direct', '--invincible', '--save-probe']),
    'bosses': (('equipment', 'room-b6c1.sav'),
               ['--rooms', *BOSS_ROOMS, '--direct', '--invincible', '--grant-equipment',
                '--boss-assist', '--boss-phase', '--save-each-room']),
    'sweep-post': (('bosses', 'room-a59f.sav'),
                   ['--sweep', '--post-boss', '--grant-equipment', '--invincible']),
    'sram-restart': (('equipment', 'station-sram.bin'), ['--save-probe']),
    'endgame': (('bosses', 'room-a59f.sav'), ['--endgame', '--invincible']),
}


def kill_tree(process):
    if sys.platform == 'win32':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    else:
        import os
        import signal
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except OSError:
            process.kill()
    process.wait()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stages = args.stages or list(STAGES)
    campaign = dict(schema='supermetroid coverage campaign v1', exe=str(args.exe.resolve()),
                    exe_sha256=sha256(args.exe), rom_sha256=sha256(args.rom),
                    interpreter_only=args.interpreter_only, stages={})
    previous = out / 'campaign.json'
    if previous.is_file():
        # Stages run by an earlier invocation stay recorded, but only for the
        # same executable and mode; a campaign never mixes builds.
        earlier = json.loads(previous.read_text())
        if (earlier.get('exe_sha256'), earlier.get('interpreter_only')) != \
                (campaign['exe_sha256'], campaign['interpreter_only']):
            raise SystemExit(f'{out} holds a campaign from a different executable or mode')
        campaign['stages'] = earlier.get('stages', {})
    for name in stages:
        seed, extra = STAGES[name]
        stage_dir = out / name
        if stage_dir.exists():
            print(f'skip {name}: {stage_dir} exists', flush=True)
            continue
        command = [sys.executable, str(QA), '--exe', str(args.exe.resolve()),
                   '--rom', str(args.rom.resolve()), '--output', str(stage_dir), *extra]
        if seed:
            source = out / seed[0] / seed[1]
            if not source.is_file():
                campaign['stages'][name] = dict(status='blocked', reason=f'missing seed {source}')
                print(f'blocked {name}: missing seed {source}', flush=True)
                continue
            command += ['--seed-sram' if source.suffix == '.bin' else '--seed-state', str(source)]
        if args.interpreter_only:
            command.append('--interpreter-only')
        started = time.monotonic()
        with (out / f'{name}.log').open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
                                       start_new_session=sys.platform != 'win32')
            try:
                returncode = process.wait(timeout=args.stage_timeout)
            except subprocess.TimeoutExpired:
                # The harness owns a game process; kill the whole tree so a
                # timed-out stage cannot leave a headless game running.
                kill_tree(process)
                returncode = 'timeout'
        campaign['stages'][name] = dict(command=command, returncode=returncode,
                                        seconds=round(time.monotonic() - started, 1))
        print(name, 'rc', returncode, flush=True)
    (out / 'campaign.json').write_text(json.dumps(campaign, indent=2) + '\n')
    summarize(out)


def summarize(out):
    sys.path.insert(0, str(ROOT / 'snesrecomp' / 'recompiler'))
    from v2.coverage_profile import load_profiles, promotion_reason
    records, stages, identities = [], {}, set()
    totals = dict(interp_cycles=0, cpu_cycles=0, interp_insns=0)
    for stage_dir in sorted(p for p in out.iterdir() if p.is_dir()):
        report_path = stage_dir / 'report.json'
        if not report_path.is_file():
            continue
        report = json.loads(report_path.read_text())
        checkpoint = stage_dir / 'tier2-profile.json'
        journal = stage_dir / 'tier2-profile.jsonl'
        source = None
        if checkpoint.is_file():
            records.append(json.loads(checkpoint.read_text()))
            source = 'checkpoint'
        elif journal.is_file():
            for line in journal.read_text().splitlines():
                if line.strip():
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass  # torn final line, as coverage_profile accepts
            source = 'journal'
        if source:
            identities.add(json.dumps(records[-1].get('identity', {}), sort_keys=True))
        execution = report.get('execution') or {}
        stats = execution.get('interp_stats') or {}
        for key in totals:
            totals[key] += int(stats.get(key, 0))
        results = report.get('results', {})
        stages[stage_dir.name] = dict(
            status=report.get('status'), error=report.get('error'),
            frame=execution.get('frame'), coverage_source=source,
            interp_cycle_pct=stats.get('interp_cycle_pct'),
            aot_cycle_pct=stats.get('aot_cycle_pct'),
            interp_cycles=stats.get('interp_cycles'), cpu_cycles=stats.get('cpu_cycles'),
            interp_insns=stats.get('interp_insns'), tier2_sites=stats.get('tier2_sites'),
            tier2_bail=stats.get('tier2_bail'),
            rooms_entered=results.get('rooms_entered'),
            skipped=results.get('skipped'),
            pickup_types_acquired=results.get('pickup_types_acquired'),
            pickups_unverified=results.get('pickups_unverified'),
            bosses_completed=results.get('bosses_completed'),
            endgame=(report.get('endgame') or {}).get('final_state'),
            # Guest-visible outcome per room, for cross-build comparison.
            outcomes={case['target']: dict(status=case['status'],
                                           after={k: case['after'][k] for k in sorted(case['after'])
                                                  if k != 'frame'} if case.get('after') else None,
                                           items=[(i['item'], i['acquired']) for i in case.get('items', [])],
                                           boss=(case.get('boss') or {}).get('defeated'))
                      for case in report.get('cases', [])},
            wram={p.name: sha256(p) for p in sorted(stage_dir.glob('*.wram'))})
    summary = dict(schema='supermetroid coverage campaign summary v1', stages=stages,
                   totals=dict(totals, interp_cycle_pct=(100.0 * totals['interp_cycles'] / totals['cpu_cycles']
                                                         if totals['cpu_cycles'] else None)))
    if totals['cpu_cycles']:
        summary['totals']['aot_cycle_pct'] = 100.0 - summary['totals']['interp_cycle_pct']
    if records:
        if len(identities) != 1:
            raise SystemExit(f'captures disagree on build identity: {sorted(identities)}')
        bundle = dict(schema='snesrecomp tier2 bundle v2', records=records)
        bundle_path = out / 'tier2-bundle.json'
        bundle_path.write_text(json.dumps(bundle, separators=(',', ':'), sort_keys=True) + '\n')
        profile = load_profiles([bundle_path])
        declared = set()
        import re
        for path in (ROOT / 'recomp').glob('bank*.cfg'):
            bank = re.fullmatch(r'bank([0-9a-fA-F]{2})', path.stem)
            if not bank:
                continue
            for line in path.read_text(encoding='utf-8').splitlines():
                match = re.match(r'\s*func\s+\S+\s+([0-9a-fA-F]+)', line)
                if match:
                    declared.add((int(bank[1], 16) << 16) | int(match[1], 16))
        reasons = {}
        for row in profile.discoveries:
            reason = promotion_reason(row, declared, profile.identity)
            reasons[reason] = reasons.get(reason, 0) + 1
        summary['profile'] = dict(path=bundle_path.name, sha256=sha256(bundle_path),
                                  identity=profile.identity, captures=len(profile.captures),
                                  discoveries=len(profile.discoveries),
                                  promotion_reasons=dict(sorted(reasons.items())),
                                  warnings=profile.warnings)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(dict(totals=summary['totals'],
                          profile={k: v for k, v in summary.get('profile', {}).items()
                                   if k in ('discoveries', 'promotion_reasons', 'warnings')},
                          stages={k: {f: v[f] for f in ('status', 'aot_cycle_pct', 'rooms_entered',
                                                         'bosses_completed', 'pickup_types_acquired')}
                                  for k, v in stages.items()}), indent=2))


def release_attract(args):
    """Free-run a shipping (no debug server) executable through attract.

    No input, no pause/step: the host runs RUN_FRAMES frames and exits, and the
    always-on post-mortem report it writes beside the executable carries the
    interpreted/AOT cycle share for the whole run ("execution_share")."""
    import os
    import shutil
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    exe = args.exe.resolve()
    report = exe.parent / 'last_run_report.json'
    if report.exists():
        report.unlink()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SM_', 'SNESRECOMP_'))}
    env.update(SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy', SDL_VIDEODRIVER='dummy',
               SDL_VIDEO_DRIVER='dummy', SNESRECOMP_RUN_FRAMES=str(args.frames),
               SNESRECOMP_TIER2_CAPTURE='1' if args.capture else '0',
               SNESRECOMP_TIER2_MANIFEST=str(out / 'tier2-profile.json'),
               SNESRECOMP_TIER2_JOURNAL=str(out / 'tier2-profile.jsonl'))
    # The same unthrottled, muted, software-rendered settings the QA harness
    # uses, so a release run and a harness run differ only in the build.
    config = out / 'config.ini'
    config.write_text(chr(10).join([
        '[General]', 'Autosave=0', 'SkipLauncher=1', 'DisableFrameDelay=1', 'RunAhead=0',
        '[Graphics]', 'WindowScale=2', 'VSync=0', 'Renderer=software', 'NewRenderer=1',
        'NoSpriteLimits=0', '[Sound]', 'EnableAudio=1', 'Volume=0', '[GamepadMap]',
        'EnableGamepad1=false', 'EnableGamepad2=false', '']), encoding='utf-8')
    with (out / 'run.log').open('wb') as log:
        process = subprocess.Popen([str(exe), '--config', str(config), '--no-launcher',
                                    str(args.rom.resolve())],
                                   cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
                                   start_new_session=sys.platform != 'win32')
        try:
            returncode = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            kill_tree(process)
            returncode = 'timeout'
    if not report.is_file():
        raise SystemExit(f'no exit report written (rc={returncode}); see {out / "run.log"}')
    shutil.copyfile(report, out / 'last_run_report.json')
    data = json.loads(report.read_text())
    result = dict(exe_sha256=sha256(exe), frames=args.frames, returncode=returncode,
                  reason=data.get('reason'), execution_share=data.get('execution_share'),
                  tier2_sites=(data.get('tier2_coverage') or {}).get('distinct_sites'))
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if not result['execution_share']:
        raise SystemExit('exit report has no execution_share section (framework too old?)')


def export_profile(campaign_dir, output):
    """Write the release coverage profile: the campaign bundle's analysis seeds.

    Instruction costs are measurements of one executable, not analysis
    seeds; discover_profile_roots never reads them, and they are ~95% of the
    bundle. RAM routine snapshots carry guest code bytes, which are derived
    from the ROM and must never enter this public repository; generation
    does not read them either. Everything the generator consumes (identity,
    capture ids, sequences, discoveries, unsafe/qualified target sets) is
    kept verbatim, and the result must yield exactly the same discoveries as
    the full bundle."""
    sys.path.insert(0, str(ROOT / 'snesrecomp' / 'recompiler'))
    from v2.coverage_profile import load_profiles
    source = campaign_dir / 'tier2-bundle.json'
    bundle = json.loads(source.read_text())
    records = []
    for record in bundle['records']:
        records.append({k: v for k, v in record.items() if k not in ('costs', 'ram_routines')})
    output.write_text(json.dumps(dict(schema='snesrecomp tier2 bundle v2', records=records),
                                 separators=(',', ':'), sort_keys=True) + '\n')
    full, slim = load_profiles([source]), load_profiles([output])
    if full.discoveries != slim.discoveries or full.identity != slim.identity \
            or full.unsafe_targets != slim.unsafe_targets \
            or full.qualified_targets != slim.qualified_targets:
        raise SystemExit('exported profile does not reproduce the campaign discoveries')
    if '"bytes"' in output.read_text():
        raise SystemExit('exported profile contains guest code bytes')
    print(json.dumps(dict(output=str(output), bytes=output.stat().st_size,
                          sha256=sha256(output), captures=len(slim.captures),
                          discoveries=len(slim.discoveries), identity=slim.identity), indent=2))


def compare(a_dir, b_dir):
    a = json.loads((a_dir / 'summary.json').read_text())
    b = json.loads((b_dir / 'summary.json').read_text())
    result = dict(a=str(a_dir), b=str(b_dir), totals=dict(a=a['totals'], b=b['totals']), stages={})
    regressions = []
    for name in sorted(set(a['stages']) | set(b['stages'])):
        sa, sb = a['stages'].get(name), b['stages'].get(name)
        if not sa or not sb:
            result['stages'][name] = dict(missing='a' if not sa else 'b')
            regressions.append(f'{name}: stage missing from {"a" if not sa else "b"}')
            continue
        differing = sorted(room for room in set(sa['outcomes']) | set(sb['outcomes'])
                           if sa['outcomes'].get(room) != sb['outcomes'].get(room))
        wram_diff = sorted(n for n in set(sa['wram']) | set(sb['wram'])
                           if sa['wram'].get(n) != sb['wram'].get(n))
        entry = dict(status=(sa['status'], sb['status']),
                     aot_cycle_pct=(sa['aot_cycle_pct'], sb['aot_cycle_pct']),
                     rooms_entered=(sa['rooms_entered'], sb['rooms_entered']),
                     bosses=(len(sa['bosses_completed'] or []), len(sb['bosses_completed'] or [])),
                     endgame=(sa.get('endgame'), sb.get('endgame')),
                     pickups=(sa['pickup_types_acquired'], sb['pickup_types_acquired']),
                     differing_outcomes=differing, differing_wram=wram_diff)
        result['stages'][name] = entry
        if sb['status'] != 'completed' and sa['status'] == 'completed':
            regressions.append(f'{name}: {sa["status"]} -> {sb["status"]} ({sb.get("error")})')
        if (sb['rooms_entered'] or 0) < (sa['rooms_entered'] or 0):
            regressions.append(f'{name}: rooms entered {sa["rooms_entered"]} -> {sb["rooms_entered"]}')
        lost = set(sa['pickup_types_acquired'] or []) - set(sb['pickup_types_acquired'] or [])
        if lost:
            regressions.append(f'{name}: pickups lost {sorted(lost)}')
        if len(sb['bosses_completed'] or []) < len(sa['bosses_completed'] or []):
            regressions.append(f'{name}: fewer bosses completed')
        if sa.get('endgame') != sb.get('endgame'):
            regressions.append(f'{name}: ending state {sa.get("endgame")} -> {sb.get("endgame")}')
    result['regressions'] = regressions
    print(json.dumps(result, indent=2))
    return 1 if regressions else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    r = sub.add_parser('run')
    r.add_argument('--exe', type=Path, required=True)
    r.add_argument('--rom', type=Path, required=True)
    r.add_argument('--output', type=Path, required=True)
    r.add_argument('--interpreter-only', action='store_true',
                   help='reference campaign with AOT bounces disabled')
    r.add_argument('--stages', nargs='*', choices=list(STAGES))
    r.add_argument('--stage-timeout', type=int, default=4 * 3600)
    b = sub.add_parser('bundle')
    b.add_argument('output', type=Path)
    c = sub.add_parser('compare')
    c.add_argument('a', type=Path)
    c.add_argument('b', type=Path)
    e = sub.add_parser('export-profile', help='write the release coverage profile from a campaign')
    e.add_argument('campaign', type=Path)
    e.add_argument('--output', type=Path, required=True)
    a = sub.add_parser('release-attract', help='measure a shipping build from its exit report')
    a.add_argument('--exe', type=Path, required=True)
    a.add_argument('--rom', type=Path, required=True)
    a.add_argument('--output', type=Path, required=True)
    a.add_argument('--frames', type=int, default=10800)
    a.add_argument('--timeout', type=int, default=1800)
    a.add_argument('--no-capture', dest='capture', action='store_false')
    args = parser.parse_args()
    if args.command == 'run':
        run(args)
    elif args.command == 'export-profile':
        export_profile(args.campaign.resolve(), args.output.resolve())
    elif args.command == 'release-attract':
        release_attract(args)
    elif args.command == 'bundle':
        summarize(args.output.resolve())
    else:
        return compare(args.a.resolve(), args.b.resolve())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
