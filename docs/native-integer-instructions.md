# Native integer instruction replacements

This change is based on KytyPS5-Native commit
47857b43743a4905eacf6b30c82f7f910ed2a093, the normal backend selected by the
SharpEmu bridge on this BC-250. The semantic audit used AnyPS5 commit
06a90922740d075d3730c5d877a979e8c4d86776, particularly IntegerDot.cpp,
GlobalLoadAddtid.cpp, the vector decoder and globalAddtid translation.

The CPU guest code and compatible GPU instructions continue to execute
natively. Ten instruction encodings outside LLVM's gfx1013 model can now
be replaced with ordinary gfx1013 instructions at shader creation:

| Encoding | Operation |
| --- | --- |
| VOP2 0x0d | signed accumulating dot4 of packed 8-bit values |
| VOP3P 0x14/0x15 | signed/unsigned dot2 of packed 16-bit values |
| VOP3P 0x16/0x17 | signed/unsigned dot4 of packed 8-bit values |
| VOP3P 0x18/0x19 | signed/unsigned dot8 of packed 4-bit values |
| GLOBAL 0x16/0x17 | lane-addressed dword load/store |
| VOP3 0x182 | readfirstlane in its native VOP1 encoding |

Integer sums wrap modulo 2^32. The implementation preserves EXEC, VCC and
SCC, supports source/destination aliases, scalar/literal inputs and DOT2
half selectors. Reducible DPP16 and DPP8 inputs are supported. A DPP source
that suppresses its write sees zero-initialized temporary data, so an
accumulating integer dot retains the original accumulator.

GLOBAL addtid uses MBCNT of an all-ones mask to obtain physical wave lane
ID, multiplies it by four and emits an ordinary GLOBAL load/store.
Offsets, scalar base, cache flags, data registers and inactive lanes retain
their original meaning. Workgroup thread ID is not substituted for lane ID.

## Loading and lifetime

The shader loader detects reachable target instructions, resolves wave size
from stage metadata and reserves four extra private VGPRs for dots and addtid. Readfirstlane
rewrites in place without extra registers. Original instruction
positions, PC-relative constants and embedded constant offsets stay intact;
branches enter appended replacement sequences and return to the next original
instruction. Patched code is immutable, GPU-mapped and retained for process
lifetime, with a 128 MiB cache budget including original-byte cache keys. PC reads require a recognized, fully
carried address calculation that resolves inside the copied blob.

The original AGC shader size and constant metadata stay unchanged. Constants
and the trailer remain at their original offsets inside the declared blob;
no bytes past shader_size are read. The shader registry records the expanded executable size. Resource
registers and shader program addresses are updated before publication.
Readfirstlane accepts an unmodified VGPR source and an ordinary SGPR destination.

A required replacement is refused atomically for unsupported modifiers,
incomplete/indirect control flow or detected non-local PC-relative data, branch range exhaustion, split
shader stages, shared VGPRs, missing stage metadata, register exhaustion,
allocation failure or cache-budget exhaustion. There is no automatic Vulkan
fallback for those refusals.

BC-250 native defaults enable KYTY_NATIVE_GPU_INTEGER_PATCH=1. PS5 Linux
defaults leave it off to retain the PS5's own ISA. Set it to 0 when launching
the emulator directly to disable replacements. Other hosts require explicit
opt-in plus native primary mode and a GEM backing device.

## Validation and limits

The CPU suite checks 26,600 integer semantic cases plus refusal, allocation
and encoding boundaries. The hardware fixtures check 4,096 lane results over 64 dispatches and execute only the generated,
opcode-scanned gfx1013 code on BC-250, in actual wave32 and wave64 modes.
They cover signed/unsigned packed widths, overflow, aliases, SGPR/literal
inputs, DPP permutations, inactive source lanes, EXEC masking, unchanged
SCC/VCC, GLOBAL loads/stores and positive/negative offsets.

These tests validate the replacement sequences. They do not prove that all
ten original raw encodings fail on the silicon, establish game compatibility
or measure performance. The opcode-table comparison is not a hardware
capability percentage.

Floating-point dot replacements remain disabled: two FMAs can differ from a
single-rounded dot. This change does not implement csub atomics, the extra
image encodings or their modifier combinations. Those candidates require separate
semantic and target-hardware validation. Unrelated shader words are unchanged.

Run CPU tests with:

    ctest --test-dir _Build/native-instructions -R 'native_gpu_(integer_patcher|shader_patcher|shader_scan)' --output-on-failure

The hardware scripts use llvm-mc, llvm-objcopy and two small test binaries;
Build and run these tests with tools/bc250-native-probe/run_integer_patch_tests.sh;
their input is synthetic and contains no game shader bytes. Results are
written under _Build/instruction-tests/hardware*.

## SharpEmu installation

The live selector is /home/david/ps5-sharp-ui-bridge-20261103/kyty-conditional-emu.sh.
ASTRO BOT and Ghost of Yotei use its normal Upstream profile, now selecting
/home/david/kyty-native-instructions-20261106/release/kyty_emulator.
It retains /home/david/kyty-upstream-clean-47857b4-20261104/release as the working
directory for existing saves, caches and assets. No GUI or game settings change.

The older optional recovery/audio executables and Fast profile retain their
original routes. Their code is not added to this native instruction build.
The original binary and selector backup remain available for rollback.
