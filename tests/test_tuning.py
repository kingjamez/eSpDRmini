"""Run with python -m unittest discover -s tests."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

import numpy as np

import dsp
import espctl
import radios


def backend(lo, mode, pll):
    esp = Mock()
    esp.command.side_effect = lambda op, arg=0: espctl.FIRMWARE_ID if arg == 0 else 0
    esp.settings.return_value = {"lo_hz": lo, "lo_mode": mode, "pll_hz": pll,
                                 "sdm_word": 0x300000, "rate": 0, "gain": 45,
                                 "rf_gain": 288, "bb_gain": 110}
    return radios.EspdrRadio(esp)


class TuningTests(unittest.TestCase):
    def test_fractional_lower_endpoint_survives_ui_rounding(self):
        radio = radios.EspdrRadio.__new__(radios.EspdrRadio)
        radio.esp = Mock()
        for mhz in (0, 1841.666667, 1841.6667):
            radio.set_lo(radio.tune_mhz(mhz) * 1e6)
            radio.esp.command.assert_called_with(espctl.ESP_SET_LO, 1841666667)
        self.assertEqual(radio.tune_mhz(3000), 2790)

    def test_normal_and_alternate_readback(self):
        for lo, mode, pll, name in ((2000000000, 2, 2400000000, "5/6"),
                                    (2439999847, 1, 2439999847, "normal")):
            with self.subTest(mode=name):
                settings = backend(lo, mode, pll).settings()
                self.assertEqual(settings["lo_hz"], lo)
                self.assertEqual(settings["lo_mode"], name)
                self.assertEqual(settings["pll_hz"], pll)

    def test_sigmf_and_spectrum_use_effective_lo(self):
        settings = backend(2000000000, 2, 2400000000).settings()
        axis = dsp.frequencies(settings["lo_hz"], settings["rate"], 2048)
        self.assertEqual(axis[1024], 2000)
        with tempfile.TemporaryDirectory() as directory:
            base = dsp.save_snapshot(directory, np.zeros(15360, np.complex64),
                                     settings, dsp.now_utc())
            meta = json.loads(Path(base + ".sigmf-meta").read_text())
        self.assertEqual(meta["captures"][0]["core:frequency"], 2000000000)
        self.assertEqual(meta["global"]["espdr:lo_mode"], "5/6")
        self.assertEqual(meta["global"]["espdr:pll_hz"], 2400000000)

    def test_espsdr_metadata_needs_no_espdr_fields(self):
        settings = {"gain": None, "agc": True}
        meta = dsp._base_meta("ci16_le", 80e6, "test", settings, None)
        self.assertNotIn("espdr:lo_mode", meta)

    def test_other_backends_keep_their_tuning_step(self):
        radio = radios.Radio()
        radio.ui_range_mhz, radio.lo_step_mhz = (2300, 5950), 1
        self.assertEqual(radio.tune_mhz(2440.3), 2440)
        self.assertEqual(radio.tune_mhz(2440.7), 2441)

    def test_frequency_command_preserves_all_32_bits(self):
        esp = espctl.Esp.__new__(espctl.Esp)
        esp._send = Mock(return_value=1)
        esp._response = Mock(return_value=(0, 0))
        esp.command(espctl.ESP_SET_LO, espctl.LO_MAX_HZ)
        self.assertEqual(esp._send.call_args_list[0].args,
                         (espctl.ESP_ARG_HIGH, espctl.LO_MAX_HZ >> 16))
        self.assertEqual(esp._send.call_args_list[1].args,
                         (espctl.ESP_SET_LO, espctl.LO_MAX_HZ))


if __name__ == "__main__":
    unittest.main()
