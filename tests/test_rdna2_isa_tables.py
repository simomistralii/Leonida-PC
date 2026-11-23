"""Consistency tests for tools/rdna2_isa_tables.py.

These tables drive every decode path in tools/rdna2_decode.py, yet no test
imports them directly: a malformed entry only surfaces when a game shader
happens to use that opcode. The checks below pin the invariants the decoder
relies on, using no game data:

* every entry is (name, operands) with a well-formed name and operand tuples
  (field, width, type): width -1 marks an implicit operand (VCC/EXEC), width 0
  marks the SIMM32 literal slot of the literal-carrying VOP2/SOPK opcodes,
  anything else must be a positive register count;
* field names are from the set the decoder's paths actually branch on - an
  unknown field name would be skipped silently by the `if fn == ...` chains,
  dropping an operand without any error;
* the literal-carrying opcodes the tables mark with a SIMM32/LITERAL field are
  exactly the ones rdna2_decode.py hard-codes in _VOP2_MANDATORY_LITERAL and
  _SOPK_MANDATORY_LITERAL;
* where an opcode exists in both a 32-bit table (VOPC/VOP2/VOP1) and the VOP3
  table at its promoted opcode (0x000/0x100/0x180 bases, as AMD's spec and the
  decoder's _e64 suffixing assume), the mnemonics must agree;
* with LEONIDA_RDNA2_ISA_XML pointing at AMD's machine-readable RDNA2 ISA
  (download line in docs/isa_coverage.md), every mnemonic must exist there
  under a matching encoding group - except the seven gfx1013 legacy/waveid
  instructions docs/isa_coverage.md says AMD's XML omits. Without the file
  this test skips, like test_isa_coverage.py's real-XML test.

Run: python -m pytest tests/test_rdna2_isa_tables.py
"""
from __future__ import annotations

import os
import re
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT, "tools")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import isa_coverage  # noqa: E402
import rdna2_decode  # noqa: E402
import rdna2_isa_tables as T  # noqa: E402

TABLES = {
    name: getattr(T, name)
    for name in (
        "DS", "FLAT", "GLOBAL", "MIMG", "MTBUF", "MUBUF", "SCRATCH", "SMEM",
        "SOP1", "SOP2", "SOPC", "SOPK", "SOPP", "VINTRP",
        "VOP1", "VOP2", "VOP3", "VOP3B", "VOP3P", "VOPC",
    )
}

# Field names the decode paths in rdna2_decode.py consume (a few only as
# implicit markers: VCC, OPR_EXEC, ATTR, VSRC, LITERAL).
KNOWN_FIELDS = {
    "ADDR", "ATTR", "DATA", "DATA0", "DATA1", "LITERAL", "OPR_EXEC",
    "SADDR", "SBASE", "SDATA", "SDST", "SIMM16", "SIMM32", "SOFFSET",
    "SRC0", "SRC1", "SRC2", "SRSRC", "SSAMP", "SSRC0", "SSRC1",
    "VADDR", "VCC", "VDATA", "VDST", "VSRC", "VSRC1",
}

NAME_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)?$")

# Real gfx1013 instructions AMD's XML omits (docs/isa_coverage.md).
GFX1013_EXTRAS = {
    "v_mac_f32", "v_mac_legacy_f32", "v_mad_f32", "v_mad_legacy_f32",
    "v_madak_f32", "v_madmk_f32", "s_get_waveid_in_workgroup",
}

REAL_XML = os.environ.get("LEONIDA_RDNA2_ISA_XML")


def test_tables_are_present_and_non_empty():
    for name, table in TABLES.items():
        assert isinstance(table, dict) and table, f"{name} missing or empty"


def test_entries_are_well_formed():
    for name, table in TABLES.items():
        for opc, entry in table.items():
            assert isinstance(opc, int) and 0 <= opc <= 0x3FF, f"{name}[{opc!r}]"
            assert isinstance(entry, tuple) and len(entry) == 2, f"{name}[{opc:#x}]"
            iname, spec = entry
            assert isinstance(iname, str) and NAME_RE.match(iname), \
                f"{name}[{opc:#x}] bad name {iname!r}"
            assert isinstance(spec, tuple), f"{name}[{opc:#x}] {iname}"
            for operand in spec:
                assert isinstance(operand, tuple) and len(operand) == 3, \
                    f"{name}[{opc:#x}] {iname}: operand {operand!r}"
                field, width, otype = operand
                assert isinstance(field, str), f"{name}[{opc:#x}] {iname}"
                if width == 0:
                    # the SIMM32 literal slot of literal-carrying opcodes
                    assert field == "SIMM32", \
                        f"{name}[{opc:#x}] {iname}: width-0 {field}"
                else:
                    assert width == -1 or width >= 1, \
                        f"{name}[{opc:#x}] {iname}: width {width}"
                assert isinstance(otype, str) and otype, \
                    f"{name}[{opc:#x}] {iname}: type {otype!r}"


def test_field_names_are_ones_the_decoder_handles():
    unknown = set()
    for name, table in TABLES.items():
        for opc, (iname, spec) in table.items():
            for field, _, _ in spec:
                if field not in KNOWN_FIELDS:
                    unknown.add((name, opc, iname, field))
    assert not unknown, f"fields the decoder skips: {sorted(unknown)!r}"


def test_names_are_unique_within_each_table():
    for name, table in TABLES.items():
        seen = {}
        for opc, (iname, _) in table.items():
            assert iname not in seen, \
                f"{name}: {iname} at both {seen[iname]:#x} and {opc:#x}"
            seen[iname] = opc


def test_literal_fields_match_the_decoder_constants():
    # The tables mark literal-carrying opcodes with a SIMM32/LITERAL operand;
    # rdna2_decode.py hard-codes the same opcodes as always carrying a
    # trailing 32-bit literal. The two must agree or decoding goes a word
    # out of sync.
    vop2_with_literal = {
        opc for opc, (_, spec) in T.VOP2.items()
        if any(field in ("SIMM32", "LITERAL") for field, _, _ in spec)
    }
    assert vop2_with_literal == set(rdna2_decode._VOP2_MANDATORY_LITERAL)
    sopk_with_literal = {
        opc for opc, (_, spec) in T.SOPK.items()
        if any(field in ("SIMM32", "LITERAL") for field, _, _ in spec)
    }
    assert sopk_with_literal == set(rdna2_decode._SOPK_MANDATORY_LITERAL)


def test_promoted_vop3_names_match_the_32bit_tables():
    # AMD's VOP3 opcode map promotes the 32-bit encodings: 0x000-0x0FF is
    # VOPC, 0x100-0x17F is VOP2, 0x180-0x1FF is VOP1. VOP2-only opcodes
    # (literal carriers, dot products) and VOP1-only opcodes legitimately
    # have no VOP3 entry; VOP3B holds the SDST forms. What must never happen
    # is the same promoted opcode carrying two different mnemonics.
    conflicts = []
    for low_name, base in (("VOPC", 0x000), ("VOP2", 0x100), ("VOP1", 0x180)):
        for opc, (iname, _) in TABLES[low_name].items():
            promoted = T.VOP3.get(base + opc) or T.VOP3B.get(base + opc)
            if promoted is not None and promoted[0] != iname:
                conflicts.append((low_name, hex(base + opc), iname, promoted[0]))
    assert not conflicts, f"VOP3 name conflicts: {conflicts!r}"


@pytest.mark.skipif(not REAL_XML or not os.path.exists(REAL_XML),
                    reason="set LEONIDA_RDNA2_ISA_XML to AMD's amdgpu_isa_rdna2.xml")
def test_mnemonics_match_amds_rdna2_spec():
    isa = isa_coverage.read_isa(REAL_XML)
    amd = isa["instructions"]
    mismatches = []
    for table_name, table in TABLES.items():
        group = "VOP3" if table_name == "VOP3B" else table_name
        for opc, (iname, _) in table.items():
            if iname in GFX1013_EXTRAS:
                continue
            groups = amd.get(iname)
            if groups is None:
                mismatches.append((table_name, hex(opc), iname, "not in AMD spec"))
            elif group not in groups:
                mismatches.append((table_name, hex(opc), iname, f"AMD groups {groups}"))
    assert not mismatches, f"{len(mismatches)} mismatches: {mismatches[:20]!r}"
