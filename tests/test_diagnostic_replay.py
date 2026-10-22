#!/usr/bin/env python3
"""ROM-backed acceptance: record actual executions, replay, and reject damage.

Requires owned ROM; optionally takes a gameplay checkpoint prepared by the
progression harness. All output, profiles and saves stay under --work-dir.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("diagnostic", ROOT / "tools/replay_diagnostic.py")
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)
CONFIG = """[General]
SkipLauncher=1
Autosave=0
RunAhead=0
DisableFrameDelay=1
[Graphics]
Fullscreen=0
WindowScale=1
VSync=0
Renderer=software
NewRenderer=1
NoSpriteLimits=1
[Sound]
EnableAudio=1
Volume=0
[GamepadMap]
EnableGamepad1=false
EnableGamepad2=false
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--phantoon-fixture", type=Path, help="living Phantoon room checkpoint before the intro")
    args = parser.parse_args()
    exe, rom, work = args.exe.resolve(strict=True), args.rom.resolve(strict=True), args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("SM_", "SNESRECOMP_", "SNES_NET"))}
    env.update(SDL_VIDEODRIVER="dummy", SDL_VIDEO_DRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
               SNESRECOMP_DEBUG_PORT="0", SNESRECOMP_EXECUTION_MODE="lle")
    env["LD_LIBRARY_PATH"] = str(exe.parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    report = dict(executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), cases=[])

    def save_report():
        (work / "report.json").write_text(json.dumps(report, indent=2))

    def run(name, command, cwd, overrides=None, expect=0):
        current = dict(env)
        current.update(overrides or {})
        start = time.monotonic()
        with (cwd / "runtime.log").open("wb") as log:
            result = subprocess.run(command, cwd=cwd, env=current, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        text = (cwd / "runtime.log").read_text(encoding="utf-8", errors="replace")
        if (expect == 0 and result.returncode != 0) or (expect != 0 and result.returncode == 0):
            raise AssertionError(f"{name}: exit={result.returncode}\n{text[-6000:]}")
        case = dict(name=name, returncode=result.returncode, seconds=time.monotonic()-start)
        report["cases"].append(case)
        save_report()
        print(name, "passed", flush=True)
        return text

    def capture(name, script, *, record=True, fixture=None, wide=False, extra=None, paced=False):
        directory = work / name
        directory.mkdir()
        (directory / "logs").mkdir()
        (directory / "config.ini").write_text(CONFIG.replace("DisableFrameDelay=1", "DisableFrameDelay=0") if paced else CONFIG)
        (directory / "input.script").write_text(script)
        (directory / "sm-video.ini").write_text(
            f"[SuperMetroidVideo]\nEnhancedRenderer={int(wide)}\nAspect=21:9\nPresentationEnabled={int(wide)}\nPresentationFPS=120\nHudAnchored=1\n")
        command = [str(exe), "--rom", str(rom), "--config", str(directory / "config.ini"),
                   "--no-launcher", "--script", str(directory / "input.script")]
        if fixture:
            # The shared host consumes --resume-state as a relaunch artifact.
            # Never hand it the caller's original checkpoint.
            shutil.copy2(fixture, directory / "resume.sav")
            command += ["--resume-state", str(directory / "resume.sav")]
            (directory / "saves").mkdir()
            shutil.copy2(fixture, directory / "saves/save1.sav")
        settings = dict(SM_REPLAY_DIR=str(directory / "logs"), SNESRECOMP_DUMP_DIR=str(directory))
        if not record:
            settings["SM_REPLAY_RECORD"] = "0"
        settings.update(extra or {})
        run(name, command, directory, settings)
        segments = sorted((directory / "logs").glob("*.replay"))
        if record and not segments:
            raise AssertionError(f"{name}: no capture")
        if not record and segments:
            raise AssertionError("opt-out still captured")
        return directory, segments

    def replay(path, name, expect=0, until=None):
        destination = work / name
        destination.mkdir()
        command = [sys.executable, str(ROOT / "tools/replay_diagnostic.py"), str(path),
                   "--exe", str(exe), "--rom", str(rom), "--headless", "--work-dir", str(destination / "reproduction")]
        if until:
            command += ["--until", str(until)]
        text = run(name, command, destination, expect=expect)
        if expect == 0 and "all recorded checkpoints matched" not in text:
            raise AssertionError(f"{name}: no verification marker\n{text[-3000:]}")
        return text

    _, boot = capture("boot", "wait 1250\ndump end\nquit\n")
    for i, segment in enumerate(boot):
        diagnostic.inspect(segment)
        replay(segment, f"boot-playback-{i}")
    # A GUI/double-click launch has no file/pipe stdout handles: the startup
    # shim must create a persistent log even before the launcher initializes.
    log_dir = exe.parent / "logs"
    existing = set(log_dir.glob("sm-*.log")) if log_dir.exists() else set()
    log_env = dict(env, SM_REPLAY_RECORD="0", SNESRECOMP_RUN_FRAMES="30")
    result = subprocess.run([str(exe), "--rom", str(rom), "--config", str(work / "boot/config.ini"),
                             "--no-launcher"], cwd=work / "boot", env=log_env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    created = set(log_dir.glob("sm-*.log")) - existing
    if result.returncode or not created or not any("host_report: Super Metroid" in p.read_text(errors="replace") for p in created):
        raise AssertionError("default session log was not created")
    report["cases"].append(dict(name="automatic-session-log", status="passed"))
    save_report()
    selected = boot[0]
    header = selected.read_text().splitlines()
    text = replay(selected, "partial-playback", until=31)
    if "matched (31 executions)" not in text:
        raise AssertionError("--until did not stop at the requested execution")

    def altered(name, change, state_change=None):
        destination = work / name
        destination.mkdir()
        path = destination / "capture.replay"
        for suffix in diagnostic.SIDECARS:
            src = selected.with_name(selected.name + suffix)
            if src.exists():
                shutil.copy2(src, path.with_name(path.name + suffix))
        path.write_text("\n".join(change(list(header))) + "\n")
        if state_change:
            p = path.with_name(path.name + ".sav")
            p.write_bytes(state_change(p.read_bytes()))
        return path

    wrong_build = altered("wrong-build-input", lambda lines: [line.replace("BUILD ", "BUILD wrong-") if line.startswith("BUILD ") else line for line in lines])
    replay(wrong_build, "wrong-build-rejected", expect=2)
    wrong_rom = altered("wrong-rom-input", lambda lines: ["ROM 00000000" if line.startswith("ROM ") else line for line in lines])
    replay(wrong_rom, "wrong-rom-rejected", expect=2)
    corrupted = altered("corrupt-state-input", lambda lines: lines, lambda data: bytes([data[0] ^ 1]) + data[1:])
    replay(corrupted, "corrupt-state-rejected", expect=2)
    malformed = altered("malformed-state-input", lambda lines: lines, lambda data: bytes([data[0] ^ 1]) + data[1:])
    lines = malformed.read_text().splitlines()
    changed = malformed.with_name(malformed.name + ".sav").read_bytes()
    i = next(i for i, line in enumerate(lines) if line.startswith("S "))
    fields = lines[i].split()
    offset, length = int(fields[1]), int(fields[2])
    fields[3] = f"{diagnostic.digest(changed[offset:offset+length]):08x}"
    lines[i] = " ".join(fields)
    malformed.write_text("\n".join(lines) + "\n")
    text = replay(malformed, "malformed-state-rejected", expect=2)
    if "incompatible machine checkpoint" not in text:
        raise AssertionError("runtime did not reject the malformed checkpoint")
    incomplete = altered("interrupted-input", lambda lines: lines[:-2])
    text = replay(incomplete, "interrupted-rejected", expect=2)
    if "incomplete checkpoint after input" not in text:
        raise AssertionError("interrupted final input was not executed")
    raster = altered("interrupted-raster-input", lambda lines: lines[:-1])
    text = replay(raster, "interrupted-raster-rejected", expect=2)
    if "incomplete raster record" not in text:
        raise AssertionError("interrupted raster was not executed")
    def mismatch(lines):
        i = next(i for i, line in enumerate(lines) if line.startswith("H "))
        values = lines[i].split()
        values[2] = f"{int(values[2], 16) ^ 1:08x}"
        lines[i] = " ".join(values)
        return lines
    text = replay(altered("mismatch-input", mismatch), "divergence-rejected", expect=2)
    if "FIRST DIVERGENCE" not in text or not (work / "divergence-rejected/reproduction/replay-divergence.sav").is_file():
        raise AssertionError("divergence evidence missing")
    if args.fixture:
        fixture = args.fixture.resolve(strict=True)
        script = "press right 120\nrenderer toggle\npress b 30\npress y 60\nrenderer toggle\npress left 120\nwait 420\ndump end\nquit\n"
        recorded, segments = capture("gameplay", script, fixture=fixture, wide=True)
        renderer_modes = {bool(int(line.split()[3],16)&8) for p in segments for line in p.read_text().splitlines() if line.startswith('I ')}
        if renderer_modes != {False, True}:
            raise AssertionError("renderer hotkey changes were not captured")
        plain, _ = capture("gameplay-unrecorded", script, record=False, fixture=fixture, wide=True)
        for suffix in ("wram.bin", "vram.bin", "sram.bin"):
            if (recorded / f"end.{suffix}").read_bytes() != (plain / f"end.{suffix}").read_bytes():
                raise AssertionError(f"diagnostics changed {suffix}")
        for i, segment in enumerate(segments):
            replay(segment, f"gameplay-playback-{i}")
        script = "press right 60\nloadstate 1\npress left 60\nloadstate 1\npress y 120\nreset\nwait 60\ndump end\nquit\n"
        _, jumped = capture("loads", script, fixture=fixture)
        if sum(line.startswith("S ") for p in jumped for line in p.read_text().splitlines()) < 3:
            raise AssertionError("loads did not create checkpoints")
        for i, segment in enumerate(jumped):
            replay(segment, f"loads-playback-{i}")
        _, bounded = capture("bounded", ("loadstate 1\nwait 2\n" * 20) + "wait 30\nquit\n", fixture=fixture,
                             extra=dict(SM_REPLAY_HISTORY_MIB="2"))
        if sum(p.with_name(p.name + ".sav").stat().st_size for p in bounded) > 2 * 1024 * 1024:
            raise AssertionError("checkpoint retention exceeded budget")
        for i, segment in enumerate(bounded):
            replay(segment, f"bounded-playback-{i}")
    if args.phantoon_fixture:
        # Observe the ROM's own AI transitions; never inject a boss phase or
        # set a success flag. The old wrong-PB bug parks all gameplay RAM just
        # before the first eye opening while host/audio frames keep advancing.
        script = ("until16 079b == cd13 1\n"
                  "until16 0fb2 == d60d 1800\ndump eye-open\n"
                  "wait 120\ndump eye-active\n"
                  "until16 0fb2 == d788 2400\ndump fireball-rain\n"
                  "wait 120\ndump after-attack\nquit\n")
        phases = ("eye-open", "eye-active", "fireball-rain", "after-attack")
        observed = []
        for label, extra in (("aot", {}), ("interpreter", {"SNESRECOMP_LLE_BOUNCE": "0"})):
            directory, segments = capture("phantoon-" + label, script, fixture=args.phantoon_fixture,
                                          record=label == "aot", wide=True, extra=extra, paced=True)
            states = [(directory / (phase + ".wram.bin")).read_bytes() for phase in phases]
            word = lambda data, address: struct.unpack_from("<H", data, address)[0]
            if word(states[0], 0xfb2) != 0xd60d or word(states[2], 0xfb2) != 0xd788:
                raise AssertionError("Phantoon did not enter the real eye and rain handlers")
            if any(word(data, 0x998) != 8 or word(data, 0xf8c) == 0 for data in states):
                raise AssertionError("Phantoon attack check lost living gameplay")
            if states[0] == states[1] or states[2] == states[3]:
                raise AssertionError("gameplay RAM froze during Phantoon attack")
            observed.append([(word(data, 0xfb2), word(data, 0xf8c), word(data, 0x9c2)) for data in states])
            for i, segment in enumerate(segments):
                replay(segment, f"phantoon-playback-{i}")
        if observed[0] != observed[1]:
            raise AssertionError(f"Phantoon phases differ from interpreter: {observed}")
    report["status"] = "passed"
    save_report()


if __name__ == "__main__":
    main()
