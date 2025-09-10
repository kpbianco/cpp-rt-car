import os
import tempfile
import unittest

from tools.differential_test import compare_outputs, max_drift


class DifferentialTestCase(unittest.TestCase):
    def test_max_drift(self):
        self.assertAlmostEqual(max_drift([0.0, 1.0], [0.1, 1.2]), 0.2)

    def test_compare_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            old_path = os.path.join(tmp, "old.txt")
            new_path = os.path.join(tmp, "new.txt")
            with open(old_path, "w", encoding="utf-8") as f:
                f.write("1.0\n2.0\n")
            with open(new_path, "w", encoding="utf-8") as f:
                f.write("1.1\n1.9\n")
            self.assertTrue(compare_outputs(old_path, new_path, threshold=0.2))
            self.assertFalse(compare_outputs(old_path, new_path, threshold=0.05))


if __name__ == "__main__":
    unittest.main()
