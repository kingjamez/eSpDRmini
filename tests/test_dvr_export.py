"""Run with python -m unittest discover -s tests."""
import datetime as dt
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

import dsp


class DvrExportTests(unittest.TestCase):
    def test_narrow_band_has_no_valid_samples(self):
        iq = np.ones(15360, np.complex64)
        with self.assertRaisesRegex(ValueError, "Widen the band"):
            dsp.extract(iq, 2440e6, 80e6, 2441, 2441.01)

    def test_rejected_export_creates_no_files(self):
        now = dt.datetime.now(dt.timezone.utc)
        settings = {"lo_hz": 2440e6, "rate": 80e6, "gain": 45}
        # The first segment fits, the second does not. Neither may be saved.
        rows = [(np.ones(n, np.complex64), settings, now) for n in (61440, 15360)]
        for separate in (False, True):
            with self.subTest(per_snapshot=separate), tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(ValueError, "Selection too narrow"):
                    dsp.save_box(directory, rows, 2441, 2441.01, per_snapshot=separate)
                self.assertEqual(list(Path(directory).iterdir()), [])

    def test_exact_filter_length_produces_one_valid_sample(self):
        fs = 80e6
        taps = dsp.box_plan(fs, 2441, 2443)["taps"]
        t = np.arange(taps) / fs
        iq = (256 * np.exp(2j * np.pi * 2e6 * t)).astype(np.complex64)
        y, plan = dsp.extract(iq, 2440e6, fs, 2441, 2443)
        self.assertEqual(len(y), 1)
        self.assertEqual(plan["samples_out"], 1)
        self.assertAlmostEqual(float(abs(y[0])), .5, places=3)

    def test_valid_tones_and_sample_counts_at_both_rates(self):
        for fs in (16e6, 80e6):
            for banks in range(1, 5):
                with self.subTest(rate=fs, banks=banks):
                    t = np.arange(15360 * banks) / fs
                    iq = (256 * np.exp(2j * np.pi * 2e6 * t)).astype(np.complex64)
                    y, plan = dsp.extract(iq, 2440e6, fs, 2441, 2443)
                    self.assertEqual(len(y), plan["samples_out"])
                    self.assertAlmostEqual(float(abs(y).mean()), .5, places=3)
                    self.assertLessEqual(len(y), len(iq) / plan["decim"])

    def test_valid_sigmf_segments_match_data(self):
        settings = {"lo_hz": 2440e6, "rate": 80e6, "gain": 45}
        now = dt.datetime.now(dt.timezone.utc)
        rows = [(np.zeros(15360, np.complex64), settings, now + dt.timedelta(seconds=k))
                for k in range(2)]
        with tempfile.TemporaryDirectory() as directory:
            out, plan, count = dsp.save_box(directory, rows, 2441, 2443)
            meta = json.loads((Path(out) / "box.sigmf-meta").read_text())
            self.assertEqual(count, 2 * plan["samples_out"])
            self.assertEqual(meta["captures"][1]["core:sample_start"], plan["samples_out"])
            self.assertEqual((Path(out) / "box.sigmf-data").stat().st_size, count * 8)

    def test_empty_and_invalid_selections(self):
        with self.assertRaisesRegex(ValueError, "at least one"):
            dsp.save_box("unused", [], 2441, 2443)
        for low, high in ((2441, 2441), (2442, 2441), (float("nan"), 2442)):
            with self.subTest(low=low, high=high), self.assertRaises(ValueError):
                dsp.box_plan(80e6, low, high, 15360)


if __name__ == "__main__":
    unittest.main()
