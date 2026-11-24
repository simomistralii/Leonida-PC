"""SHADER-1266: with tfe (or lwe) an image_load returns the fetch status in the dword after its data.

RDNA2 ISA, MIMG TFE/LWE: the data takes popcount(dmask) VGPRs (half that, rounded up, under d16) and the status follows
in the next one. The translator binds only fully resident resources, so it writes 0 there; without tfe/lwe the next
VGPR is not touched. The GPU check is runtime/test/shaders/cs_imgload_tfe.asm and cs_imgload_d16_tfe.asm (exact on
llvmpipe). Hand-written programs only; no game data.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

import support

SPIRV_AS = shutil.which("spirv-as")
SPIRV_VAL = shutil.which("spirv-val")


def program(vdata, mods):
    # The would-be status VGPR holds 1000 before the load, so a stale status is visible in the store list.
    hi = vdata[1]
    return [f"v_mov_b32_e32 v{hi}, 1000",
            f"image_load v[{vdata[0]}:{hi}], v[1:2], s[16:23] {mods} dim:SQ_RSRC_IMG_2D unorm",
            f"exp mrt0 v{vdata[0]}, v{hi}, v{hi}, v{hi} done vm",
            "s_endpgm"]


def stores(spvasm, reg):
    return [line.split()[-1] for line in spvasm.splitlines() if line.strip().startswith(f"OpStore %reg_v{reg} ")]


class ImageLoadTfeStatus(unittest.TestCase):
    CASES = (  # (dmask, d16, first data VGPR, status VGPR)
        (0x1, False, 7, 8), (0x3, False, 7, 9), (0x7, False, 7, 10), (0xF, False, 7, 11),
        (0x1, True, 7, 8), (0x3, True, 7, 8), (0x7, True, 7, 9), (0xF, True, 7, 9),
    )

    def test_status_dword_is_zero_after_the_data(self):
        for dmask, d16, first, status in self.CASES:
            for flag in ("tfe", "lwe"):
                mods = f"dmask:{dmask:#x} {flag}" + (" d16" if d16 else "")
                with self.subTest(mods=mods):
                    spvasm = support.translate_lines("ps_tfe_status", program((first, status), mods))
                    self.assertEqual(stores(spvasm, status), ["%c3e8", "%c0"])

    def test_without_tfe_the_next_vgpr_keeps_its_value(self):
        for dmask, d16, first, status in self.CASES:
            mods = f"dmask:{dmask:#x}" + (" d16" if d16 else "")
            with self.subTest(mods=mods):
                spvasm = support.translate_lines("ps_tfe_status", program((first, status), mods))
                self.assertEqual(stores(spvasm, status), ["%c3e8"])

    @unittest.skipUnless(SPIRV_AS and SPIRV_VAL, "needs spirv-as and spirv-val")
    def test_modules_validate(self):
        for dmask, d16, first, status in self.CASES:
            mods = f"dmask:{dmask:#x} tfe" + (" d16" if d16 else "")
            with self.subTest(mods=mods), tempfile.TemporaryDirectory() as tmp:
                spvasm = support.translate_lines("ps_tfe_status", program((first, status), mods))
                spv = os.path.join(tmp, "m.spv")
                subprocess.run([SPIRV_AS, "--target-env", "vulkan1.2", "-o", spv, "-"], input=spvasm, text=True,
                               check=True)
                r = subprocess.run([SPIRV_VAL, "--target-env", "vulkan1.2", spv], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
