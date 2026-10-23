"""Harvested coverage: the release recipe's profiles load, publish nothing
ROM-derived, and the campaign tooling exports and compares faithfully."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'snesrecomp' / 'recompiler'))
from v2.coverage_profile import load_profiles  # noqa: E402

spec = importlib.util.spec_from_file_location('campaign', ROOT / 'tools/sm_coverage_campaign.py')
campaign = importlib.util.module_from_spec(spec)
spec.loader.exec_module(campaign)

ROM_SHA = '12b77c4bc9c1832cee8881244659065ee1d84c70c3d29e6eaf92e6798cc2ca72'
IDENTITY = dict(rom_sha256=ROM_SHA, module_id='main', mapper='lorom',
                program_digest='a' * 64, build_digest='b' * 64)


def checkpoint(capture, discoveries, costs=(), ram=()):
    return dict(schema='snesrecomp tier2 coverage v2', capture_id=capture, sequence=7,
                identity=IDENTITY, checkpoint_complete=True, discoveries=list(discoveries),
                costs=list(costs), ram_routines=list(ram))


def row(target, kind='call_gap', **counts):
    value = dict(site_pc24='0x808000', target_pc24=target, entry_mx='M0X0', emulation=0,
                 site_kind=kind, fallback_reason='missing_entry', observed_hits=1,
                 completed_hits=0, yielded_hits=0, pending_hits=0, bail_hits=0, sequence=3,
                 first_frame=0, last_frame=9)
    value.update(counts)
    return value


class ReleaseRecipeProfiles(unittest.TestCase):
    def test_every_historical_profile_exists_loads_and_is_publishable(self):
        recipe = json.loads((ROOT / 'profiles/release.json').read_text())
        self.assertEqual(recipe['rom_sha256'], ROM_SHA)
        self.assertTrue(recipe['historical_profiles'], 'release ships without harvested coverage')
        for name in recipe['historical_profiles']:
            path = ROOT / name
            self.assertTrue(path.is_file(), name)
            profile = load_profiles([path], expected_rom=ROM_SHA, expected_module='main',
                                    legacy_rom=ROM_SHA)
            self.assertTrue(profile.discoveries, name)
            text = path.read_text()
            # Costs are per-build measurements; RAM snapshots carry ROM bytes.
            self.assertNotIn('"bytes"', text, name)
            self.assertNotIn('"costs"', text, name)


class CampaignTooling(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_export_keeps_every_seed_and_drops_costs_and_rom_bytes(self):
        records = [
            checkpoint('cap-a', [row('0x848000'), row('0x8C9000', kind='goto_gap')],
                       costs=[dict(record_kind='instruction', target_pc24='0x848000',
                                   entry_mx='M0X0', emulation=0, interpreted_instructions=4,
                                   guest_cycles=12)],
                       ram=[dict(entry_pc24='0x7E2000', hash='0x1', entry_mx='M1X1',
                                 emulation=0, bytes='A9006B')]),
            checkpoint('cap-b', [row('0x8B9000', bail_hits=1)]),
        ]
        (self.dir / 'tier2-bundle.json').write_text(json.dumps(
            dict(schema='snesrecomp tier2 bundle v2', records=records)))
        output = self.dir / 'profile.json'
        campaign.export_profile(self.dir, output)
        text = output.read_text()
        self.assertNotIn('"bytes"', text)
        self.assertNotIn('"costs"', text)
        full = load_profiles([self.dir / 'tier2-bundle.json'])
        slim = load_profiles([output])
        self.assertEqual(full.discoveries, slim.discoveries)
        self.assertEqual(full.captures, slim.captures)
        # A bailed observation is kept: it is a conservative exclusion.
        self.assertTrue(any(r['bail_hits'] for r in slim.discoveries))

    def test_compare_reports_lost_rooms_pickups_bosses_and_endings(self):
        def summary(rooms, pickups, bosses, ending):
            stage = dict(status='completed', error=None, aot_cycle_pct=80.0, rooms_entered=rooms,
                         pickup_types_acquired=pickups, bosses_completed=bosses, endgame=ending,
                         outcomes={}, wram={})
            return dict(schema='supermetroid coverage campaign summary v1',
                        stages=dict(route=stage), totals={})
        a, b = self.dir / 'a', self.dir / 'b'
        a.mkdir()
        b.mkdir()
        (a / 'summary.json').write_text(json.dumps(summary(5, ['Morph', 'Bombs'], [1], 39)))
        (b / 'summary.json').write_text(json.dumps(summary(4, ['Morph'], [], 26)))
        self.assertEqual(campaign.compare(a, b), 1)
        (b / 'summary.json').write_text(json.dumps(summary(5, ['Morph', 'Bombs'], [1], 39)))
        self.assertEqual(campaign.compare(a, b), 0)


if __name__ == '__main__':
    unittest.main()
