#!/usr/bin/env python3
"""Record/replay the actual faithful runtime under a non-off policy label."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--policy', choices=('off', 'on', 'force', 'verify', 'auto', 'lle', 'hle'), default='on')
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='sm-policy-replay-', dir=args.artifacts.resolve()))
    config = root/'config.ini'
    config.write_text('[General]\nAutosave=0\nSkipLauncher=1\nDisableFrameDelay=1\nRunAhead=0\n'
        '[Graphics]\nRenderer=software\nFullscreen=0\nNewRenderer=1\n'
        '[Sound]\nEnableAudio=1\nVolume=0\n[Controller]\nSourceP1=0\nSourceP2=0\n'
        '[GamepadMap]\nEnableGamepad1=false\nEnableGamepad2=false\n')
    env = {k:v for k,v in os.environ.items() if not k.startswith(('SM_', 'SNESRECOMP_', 'SDL_', 'SNES_NET'))}
    env.update(SDL_AUDIO_DRIVER='dummy', SDL_AUDIODRIVER='dummy',
               SDL_VIDEO_DRIVER='dummy', SDL_VIDEODRIVER='dummy',
               SNESRECOMP_EXECUTION_MODE=args.policy, SNESRECOMP_DEBUG_PORT='0',
               SM_RUN_FRAMES='900', SM_REPLAY_RECORD='1', SM_REPLAY_DIR=str(root/'recordings'))
    command = [str(args.exe.resolve()), '--rom', str(args.rom.resolve()), '--config', str(config), '--no-launcher']
    def run(command, env, directory):
        directory.mkdir(exist_ok=True)
        with (directory/'runtime.log').open('wb') as log:
            result = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
        assert result.returncode == 0, (result.returncode, directory)
        return (directory/'runtime.log').read_text(errors='replace')
    run(command, env, root/'record')
    recordings = sorted((root/'recordings').glob('*.replay'))
    assert recordings, 'policy prevented faithful recording'
    env.pop('SM_RUN_FRAMES'); env['SM_REPLAY_RECORD'] = '0'
    results = []
    for number, replay in enumerate(recordings):
        records = replay.read_text().splitlines()
        frames = sum(line.startswith('I ') for line in records)
        assert records[3] == 'MODE lle'
        log = run(command + ['--replay-diagnostic', str(replay)], env, root/f'play-{number}')
        assert f'all recorded checkpoints matched ({frames} executions)' in log, replay
        results.append({'replay': replay.name, 'executions': frames, 'matched': True})
    report = {'passed': True, 'policy': args.policy, 'recordings': results}
    (root/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
