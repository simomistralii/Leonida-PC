"""Synthetic coverage of the report join and its ambiguous/missing evidence."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import world_drawable_lookup as lookup
import world_names


class DrawableLookupTest(unittest.TestCase):
    def test_only_export_candidates_with_consistent_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ["one", "many", "conflict", "wrong_key", "unaligned", "cached", "missing_key"]
            definitions, streams, placements = [], [], []
            for i, name in enumerate(names):
                h = world_names.joaat(name)
                definitions.append({"name": h, "asset_name": h, "asset_type": 2})
                key = world_names.hash64(name)
                placements.append({"archetype": f"{h:x}", "key": f"{key:x}", "position": [0, 0, 0],
                                   "rotation": [0, 0, 0, 1], "lod_level": 6})
                streams.append({"offset": 4096 * i + 16, "names": [name + ".#dr"]})
            streams.append({"offset": 4096 * 8 + 16, "names": ["many.#dr"]})
            definitions.append(dict(definitions[2], asset_type=1))
            placements[3]["key"] = "0"
            streams[4]["offset"] += 1
            placements[-1].pop("key")
            for file, data in [("defs", {"archetypes": definitions}),
                               ("names", {"summary": {"archive": "test.rpf"}, "files": streams}),
                               ("manifest", {"Items": [{"Name": "cached"}]})]:
                (root / file).write_text(json.dumps(data), encoding="utf-8", newline="\n")
            (root / "entities").write_text("\n".join(map(json.dumps, placements)), encoding="utf-8", newline="\n")
            result = lookup.lookup(root / "entities", root / "defs", root / "names", [root / "manifest"])
            self.assertEqual(result["outcomes"], {"candidate": 1, "ambiguous": 1, "unresolved": 3,
                                                  "listed_in_manifest": 1, "missing_or_invalid_key": 1})
            self.assertEqual(sorted(r["offsets"] for r in result["candidates"]), [[16], [4112, 32784]])


if __name__ == "__main__":
    unittest.main()
