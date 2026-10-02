"""Run with python -m unittest discover -s tests."""
import io
import struct
import unittest
from unittest.mock import Mock, patch
import zlib

import espctl
import radios


class FirmwareDetectionTests(unittest.TestCase):
    def test_capability_probe_rejects_streaming_without_capturing(self):
        for status, supported in ((1, False), (2, True), (4, True), (5, False)):
            with self.subTest(status=status):
                esp = espctl.Esp.__new__(espctl.Esp)
                esp.seq = 0
                head = struct.pack("<BBBBHHI", espctl.RESP_MAGIC, 1,
                                   espctl.ESP_SNAPSHOT, status, 1, 0, 0)
                incoming = io.BytesIO(head + struct.pack("<I", zlib.crc32(head)))
                esp.ser = Mock(read=incoming.read)
                self.assertEqual(esp.supports_snapshot(), supported)
                packet = esp.ser.write.call_args.args[0]
                magic, op, arg, seq = struct.unpack("<BBHH", packet[:6])
                self.assertEqual((magic, op, arg, seq),
                                 (espctl.REQ_MAGIC, espctl.ESP_SNAPSHOT, 5, 1))
                self.assertEqual(struct.unpack("<I", packet[6:])[0], zlib.crc32(packet[:6]))

    def test_matching_id_without_snapshot_loads_new_firmware(self):
        streaming, loaded = Mock(), Mock()
        streaming.command.return_value = espctl.FIRMWARE_ID
        streaming.supports_snapshot.return_value = False
        with patch.object(espctl, "Esp", side_effect=[streaming, loaded]), \
                patch.object(espctl, "load_ram") as load, \
                patch.object(radios, "EspdrRadio") as wrapper:
            radios.open_radio("test-port", backend="espdr")
        streaming.close.assert_called_once()
        load.assert_called_once_with("test-port")
        wrapper.assert_called_once_with(loaded)

    def test_snapshot_firmware_is_reused(self):
        esp = Mock()
        esp.command.return_value = espctl.FIRMWARE_ID
        esp.supports_snapshot.return_value = True
        with patch.object(espctl, "Esp", return_value=esp), \
                patch.object(espctl, "load_ram") as load, \
                patch.object(radios, "EspdrRadio") as wrapper:
            radios.open_radio("test-port", backend="espdr")
        load.assert_not_called()
        esp.close.assert_not_called()
        wrapper.assert_called_once_with(esp)
        self.assertEqual(esp.ser.timeout, 5)

    def test_failed_probe_closes_port_before_loader(self):
        for failure in ("identity", "capability"):
            with self.subTest(failure=failure):
                esp = Mock()
                esp.command.return_value = espctl.FIRMWARE_ID
                method = esp.command if failure == "identity" else esp.supports_snapshot
                method.side_effect = espctl.ControlError("timeout")
                with patch.object(espctl, "Esp", return_value=esp):
                    self.assertIsNone(radios._probe_espdr("test-port"))
                esp.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
