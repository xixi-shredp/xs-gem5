import sys
from pathlib import Path
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_h2p_output import parse_h2p_output


class H2POutputTest(unittest.TestCase):
    def parse(self, contents):
        with tempfile.TemporaryDirectory() as tmpdir:
            output = Path(tmpdir) / "h2p-indirect.txt"
            output.write_text(contents, encoding="utf-8")
            return parse_h2p_output(output)

    def test_accepts_empty_output(self):
        self.assertEqual(self.parse(""), {})

    def test_parses_sorted_records(self):
        self.assertEqual(
            self.parse(
                "0x1000, 3, 0x2000, 0x3000\n"
                "0x1100, 1, 0x4000\n"
            ),
            {
                0x1000: (3, (0x2000, 0x3000)),
                0x1100: (1, (0x4000,)),
            },
        )

    def test_rejects_unsorted_pc(self):
        with self.assertRaisesRegex(ValueError, "PCs must be strictly increasing"):
            self.parse("0x1100, 1, 0x2000\n0x1000, 1, 0x3000\n")

    def test_rejects_unsorted_or_duplicate_targets(self):
        for targets in ("0x3000, 0x2000", "0x2000, 0x2000"):
            with self.subTest(targets=targets):
                with self.assertRaisesRegex(
                    ValueError, "targets must be unique and increasing"
                ):
                    self.parse(f"0x1000, 1, {targets}\n")

    def test_rejects_zero_misses_and_missing_target(self):
        with self.assertRaisesRegex(ValueError, "positive decimal"):
            self.parse("0x1000, 0, 0x2000\n")
        with self.assertRaisesRegex(ValueError, "expected PC"):
            self.parse("0x1000, 1\n")


if __name__ == "__main__":
    unittest.main()
