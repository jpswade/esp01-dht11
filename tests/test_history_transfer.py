import importlib.util
from pathlib import Path
import math
import struct
import unittest

spec=importlib.util.spec_from_file_location("transfer",Path(__file__).resolve().parents[1]/"history_transfer.py")
transfer=importlib.util.module_from_spec(spec)
spec.loader.exec_module(transfer)


class HistoryTransferTests(unittest.TestCase):
    def snapshot(self, points, captured=1000):
        return {"captured_unix_s":captured,"history":{"readings":[
            {"age_ms":age,"humidity_percent":h,"temperature_c":t} for age,h,t in points]}}

    def test_preserves_points_and_advances_ages(self):
        data=transfer.encode_history([self.snapshot([(300000,45,20),(0,None,None)])],1010)
        self.assertEqual(data[:4],b"HST1")
        self.assertEqual(struct.unpack_from('<I',data,4)[0],2)
        self.assertEqual(struct.unpack_from('<Iff',data,8),(310000,45,20))
        self.assertTrue(math.isnan(struct.unpack_from('<Iff',data,20)[1]))

    def test_merges_startup_samples_and_limits_capacity(self):
        old=self.snapshot([(i*300000,45,20) for i in range(288,-1,-1)])
        new=self.snapshot([(0,46,21)],captured=1010)
        data=transfer.encode_history([old,new],1010)
        count=struct.unpack_from('<I',data,4)[0]
        self.assertEqual(count,289)
        self.assertEqual(struct.unpack_from('<Iff',data,8+(count-1)*12),(0,46,21))
        self.assertEqual(len(data),8+12*289)

    def test_expired_points_drop_and_invalid_values_fail(self):
        data=transfer.encode_history([self.snapshot([(86400000,45,20)])],1010)
        self.assertEqual(struct.unpack_from('<I',data,4)[0],0)
        for value in (101,-1,float('inf')):
            with self.assertRaises(ValueError):
                transfer.encode_history([self.snapshot([(0,value,20)])],1000)
