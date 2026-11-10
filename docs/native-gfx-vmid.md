# Native graphics VMID ownership

The native backend configures PS5 private/shared memory apertures and, when requested, per-VMID accumulation registers. These settings must belong to the VMID that executes its jobs. Following the first cached PASID match after each flip could restore a slot while later work still used it.

When PS5 per-VMID graphics state is requested, native startup now reserves a graphics-hub VMID for its DRM VM. It submits only the existing shadow-off tail and waits for completion, establishing the PASID mapping before any title shader runs. Startup accepts a unique graphics mapping, sets the requested registers, and verifies their values by readback. No flip callback moves or restores the graphics state during execution. The reservation remains with the DRM VM; normal cleanup restores the helper's register writes.

This is a shared native default. It has no title ID, shader hash, game graphics-mode check or experimental enable flag. Existing explicit low-level configurations that request no per-VMID graphics state do not require this setup. KFD queues retain their separate initialization.

If reservation, priming, PASID ownership or register verification fails, startup exits with code 78 before title GPU work rather than continuing with unverifiable apertures. An unavailable or ambiguous reserved slot is a startup error; the IH LUT is a cache and must not be treated as an executing-VMID register.

The CPU tests in `tests/NativeGpuWatchTests.cpp` cover every graphics slot, multiple cached matches, reassignment, unknown/zero PASIDs, and exclusion of kernel, KFD and out-of-range IDs. They can run without DRM, KFD or the GPU helper:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -O2 -Isrc tests/NativeGpuWatchTests.cpp -o /tmp/native_watch_cpu_tests
/tmp/native_watch_cpu_tests
```
