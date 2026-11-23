import sys
import os
import unittest

sys.path.insert(0, 'tools')
import rdna2spirv
import spirv_asm

class MeshAllocSingleWriterTests(unittest.TestCase):
    def test_ms_alloc_req_single_writer_guard(self):
        """s_sendmsg(MSG_GS_ALLOC_REQ) writes %ms_counts race-free from whichever wave sends it.

        SHADER-766 made the write race-free by letting only local_index 0 store it, but a shader may send ALLOC_REQ
        from another wave only (runtime/test/shaders/ms_wave_info_s3.asm), and then the counts stayed 0 and nothing
        was drawn (#6172). Every invocation that sends the message now stores the uniform m0 with a relaxed
        Workgroup-scope OpAtomicStore: atomics do not race, and nothing ties the write to invocation 0."""
        for mesh_compute in (False, True):
            orig_mc = rdna2spirv.MESH_COMPUTE
            try:
                rdna2spirv.MESH_COMPUTE = mesh_compute
                gen = rdna2spirv.Gen("ms")
                gen.ms = {"threads": 128, "maxv": 64, "maxp": 64, "clip": 0, "cull": 0, "params": []}
                # Emit MSG_GS_ALLOC_REQ
                ins = {
                    "op": "s_sendmsg",
                    "args": [{"k": "special", "t": "sendmsg(MSG_GS_ALLOC_REQ)"}],
                    "mods": []
                }
                gen._insn(ins)

                body_text = "\n".join(gen.body)
                # 1. The counts are written with an atomic store of m0, Workgroup scope (2), Relaxed (0).
                self.assertIn("= OpLoad %uint %m0", body_text)
                self.assertRegex(body_text, r"OpAtomicStore %ms_counts %c2 %c0 %\w+")
                # 2. No plain store: two waves storing at once would be SharedMemoryDataRace-RaceOnStore.
                self.assertNotIn("OpStore %ms_counts", body_text)
                # 3. Not tied to one invocation or one wave.
                self.assertNotIn("%local_index", body_text)
                self.assertNotIn("OpBranchConditional", body_text)
                self.assertTrue(gen.ms_alloc)

                # 5. Verify assembly into a complete module passes spirv_asm
                # Build mock header
                types_and_vars = [
                    "OpCapability Shader",
                    "OpCapability MeshShadingEXT" if not mesh_compute else "",
                    "OpMemoryModel Logical GLSL450",
                    'OpEntryPoint GLCompute %main "main" %local_index' if mesh_compute else 'OpEntryPoint MeshEXT %main "main" %local_index',
                    "OpExecutionMode %main LocalSize 128 1 1",
                    "OpDecorate %local_index BuiltIn LocalInvocationIndex",
                    "%void = OpTypeVoid",
                    "%bool = OpTypeBool",
                    "%uint = OpTypeInt 32 0",
                    "%c0 = OpConstant %uint 0",
                    "%c2 = OpConstant %uint 2",
                    "%_ptr_Input_uint = OpTypePointer Input %uint",
                    "%_ptr_Workgroup_uint = OpTypePointer Workgroup %uint",
                    "%_ptr_Function_uint = OpTypePointer Function %uint",
                    "%local_index = OpVariable %_ptr_Input_uint Input",
                    "%ms_counts = OpVariable %_ptr_Workgroup_uint Workgroup",
                    "%m0 = OpVariable %_ptr_Function_uint Function",
                    "%fn_t = OpTypeFunction %void",
                    "%main = OpFunction %void None %fn_t",
                    "%entry = OpLabel"
                ]
                spv_text = "\n".join([line for line in types_and_vars if line]) + "\n" + body_text + "\nOpReturn\nOpFunctionEnd\n"
                bin_data = spirv_asm.assemble(spv_text)
                self.assertGreater(len(bin_data), 0)
            finally:
                rdna2spirv.MESH_COMPUTE = orig_mc

if __name__ == '__main__':
    unittest.main()
