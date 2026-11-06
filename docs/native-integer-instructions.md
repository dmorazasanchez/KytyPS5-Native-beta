# Native shader instruction replacements

Base: KytyPS5-Native 47857b43743a4905eacf6b30c82f7f910ed2a093.
Semantic reference: AnyPS5 06a90922740d075d3730c5d877a979e8c4d86776,
including IntegerDot, GlobalLoadAddtid, BufferAtomicCsub, Float16Dot,
F32Dot2Rounding and SpirvAluEmitterFloat64. The exact floating arithmetic is
adapted under the repositories' GPL-2.0 licenses; the source identifies its origin.

Compatible CPU and GPU instructions continue to run natively. Fourteen
candidate encodings now have replacements made from ordinary gfx1013 instructions:

| Encoding | Operation |
| --- | --- |
| VOP2 0x0d | signed accumulating dot4 of packed 8-bit values |
| VOP3P 0x14/0x15 | signed/unsigned dot2 of packed 16-bit values |
| VOP3P 0x16/0x17 | signed/unsigned dot4 of packed 8-bit values |
| VOP3P 0x18/0x19 | signed/unsigned dot8 of packed 4-bit values |
| GLOBAL 0x16/0x17 | lane-addressed dword load/store |
| VOP3 0x182 | readfirstlane in its native VOP1 encoding |
| MUBUF/GLOBAL 0x34 | unsigned saturating subtraction atomics |
| VOP2 0x02, VOP3P 0x13 | exact half dot2 plus binary32 accumulator |

## Semantics

Integer dots wrap modulo 2^32 and support aliases, scalar/literal inputs,
half selectors and reducible DPP16/DPP8. Zero-initialized DPP scratch
preserves the accumulator when the source lane suppresses a write.
GLOBAL addtid uses physical wave lane ID from MBCNT, multiplied by four.
Offsets, cache flags, scalar bases and inactive lanes retain their meanings.
Readfirstlane maps directly to native VOP1 without added registers.

Buffer/GLOBAL csub uses native CMPSWAP retry loops: max(old-sub, 0), with
unsigned comparison and linearizable shared-address updates. GLC returns
the old value; no-return forms preserve the original data/destination.
EXEC, VCC, SCC and borrowed ordinary SGPRs are restored. Five SGPRs are
spilled into private VGPR lanes using EXEC-independent WRITELANE/READLANE.
Explicit memory/dependency waits protect register lifetimes. Four extra
VGPRs are needed. LDS/TFE and unsupported scalar-offset forms refuse.

Floating dot2 uses exact half conversion, binary64 TwoSum expansions and
round-to-odd before one binary32 rounding. It implements signed zero,
cancellation, half subnormals, the flushed binary32 accumulator, infinities,
and NaN payload priority from the pinned AnyPS5 exact model.
Scalar/literal sources, aliases, half selectors and A/B sign modifiers are
supported; float inline operands use binary16 bits. DPP/SDWA, clamp and
accumulator modifiers refuse. MODE, VCC, SCC and borrowed SGPRs are restored.
The current sequence needs 42 private VGPRs. This is a correctness path;
game performance is unmeasured.

## Loader and control

Passes compose transactionally. Original code/data offsets remain fixed;
replacement stubs are appended and branch back. The AGC declared shader
blob includes constants/trailer data and is never read beyond its size.
Immutable GPU-mapped code is retained for process lifetime within a
128 MiB cache budget. The cache stores actual scratch usage; resource
registers use the correct wave32/wave64 granularity on first use and hits.

Unsupported operands, non-local/indirect control flow, relative VGPR indexing
with added registers, branch exhaustion, shared VGPRs, split stages,
missing metadata, register exhaustion and mapping/budget failures refuse.
There is no automatic Vulkan fallback.

BC-250 native defaults enable KYTY_NATIVE_GPU_INTEGER_PATCH=1; this legacy
control now gates all 14 replacements. Direct launches can set it to 0.
PS5 Linux leaves it off to retain the PS5 ISA. Other devices require
explicit native primary mode and GEM support.

## Verification

- Five CTest suites: integer, atomic, float, original shader patcher, scanner.
- 26,600 integer CPU cases, 512 atomic wave executions, 42,576 floating
  exact CPU cases plus 2,000 accumulating-form checks.
- 348 synthetic GPU dispatches / 22,272 lane results, all matching:
  original 64 re-passed; 202 atomics; 82 floating.
- Actual wave32/wave64, aliases, masks including zero EXEC, contention,
  returned/no-return atomics, flags/SGPR/MODE restoration, selectors,
  scalar/literal inputs, signed offsets and relocated constants.

Run the five CPU suites with ctest -R
'native_gpu_(integer_patcher|atomic_patcher|float_patcher|shader_patcher|shader_scan)'.
Run tools/bc250-native-probe/run_native_patch_tests.sh for full reproduction.
Hardware tests execute only generated opcode-scanned native programs;
no original unsupported encoding was submitted. These tests do not prove
physical capability of original encodings, game compatibility or performance.
Detailed records: docs/native-integer-validation.json.

## Image candidates still pending

The remaining 26 inventory entries are image operations. Eight BY/PCK stores
are explicitly unimplemented in the pinned AnyPS5 translator. Decoder
presence does not establish a completed implementation.
Four BY-load experiments are isolated under
tools/bc250-native-probe/experimental-image-by and are disabled.
A two-component R32_UINT read exposed a native missing-channel result
different from the AnyPS5/Vulkan model; nonzero mip/base-level and other
descriptor cases need validation. Adjust sampling, packed gathers and PCK
loads require their own descriptor/format/sampler handling.

## SharpEmu installation

The normal Upstream profiles for ASTRO BOT and Ghost of Yotei select
/home/david/kyty-native-instructions-20261106/release/kyty_emulator through
/home/david/ps5-sharp-ui-bridge-20261103/kyty-conditional-emu.sh.
Working directory remains the original 47857b4 release directory for saves,
caches and assets. Yotei's GHOST2APP0000000 / GHOST2BASE000000 registrations
remain in the selector. Fast and old optional recovery/audio routes retain
their original executables. The original and ten-replacement binaries are
retained for comparison/rollback. No issue was published.
