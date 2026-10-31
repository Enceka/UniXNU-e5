import importlib.util
from pathlib import Path
import struct
import unittest
import uuid

spec = importlib.util.spec_from_file_location('data_image', Path(__file__).parents[1] / 'check-data-image.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
UUID = '30ADC749-351F-4FFF-998A-20E1B683859F'
STALE_UUID = 'B5A22774-89CF-41E5-A62B-5AFA6B2710BC'


def seal(image, offset):
    struct.pack_into('<Q', image, offset, module.checksum(image[offset:offset + 4096]))


def fixture():
    image = bytearray(8192)
    struct.pack_into('<QQI', image, 8, 1, 6, 0x80000001)
    image[32:36] = b'NXSB'
    struct.pack_into('<IQ', image, 36, 4096, 2)
    struct.pack_into('<QQI', image, 4096 + 8, 1026, 6, 13)
    image[4096 + 32:4096 + 36] = b'APSB'
    image[4096 + 0xF0:4096 + 0x100] = uuid.UUID(UUID).bytes
    struct.pack_into('<H', image, 4096 + 0x3C4, 0x40)
    seal(image, 0)
    seal(image, 4096)
    return image


class DataImageTests(unittest.TestCase):
    def test_valid_fixture(self):
        self.assertEqual(module.check(fixture(), UUID, 8192)['volume_uuid'], UUID)

    def test_stale_driver_uuid(self):
        with self.assertRaisesRegex(ValueError, 'Data UUID .* != driver UUID'):
            module.check(fixture(), STALE_UUID, 8192)

    def test_truncated_window(self):
        with self.assertRaisesRegex(ValueError, 'window size'):
            module.check(fixture()[:-1], UUID, 8192)

    def test_container_exceeds_window(self):
        image = fixture()
        struct.pack_into('<Q', image, 40, 3)
        seal(image, 0)
        with self.assertRaisesRegex(ValueError, 'truncated APFS'):
            module.check(image, UUID, 8192)

    def test_container_checksum(self):
        image = fixture()
        image[100] ^= 1
        with self.assertRaisesRegex(ValueError, 'NXSB checksum'):
            module.check(image, UUID, 8192)

    def test_volume_checksum(self):
        image = fixture()
        image[4096 + 100] ^= 1
        with self.assertRaisesRegex(ValueError, 'APSB checksum'):
            module.check(image, UUID, 8192)

    def test_wrong_role(self):
        image = fixture()
        struct.pack_into('<H', image, 4096 + 0x3C4, 1)
        seal(image, 4096)
        with self.assertRaisesRegex(ValueError, 'not Data'):
            module.check(image, UUID, 8192)

    def test_missing_volume(self):
        image = fixture()
        image[4096:8192] = bytes(4096)
        with self.assertRaisesRegex(ValueError, 'no checksummed Data'):
            module.check(image, UUID, 8192)

    def test_future_transaction(self):
        image = fixture()
        struct.pack_into('<Q', image, 4096 + 16, 7)
        seal(image, 4096)
        with self.assertRaisesRegex(ValueError, 'transaction outside'):
            module.check(image, UUID, 8192)


if __name__ == '__main__':
    unittest.main()
