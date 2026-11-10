// Optional capture/upscaler observers are disabled in the stock-renderer build.
#include "graphics/native_gpu/nativePassCapture.h"
#include "graphics/native_gpu/nativeUpscaler.h"
namespace NativePassCapture {
bool Active() { return false; }
void Submit(const Pass&) {}
void NoteFlip(int) {}
}
namespace NativeUpscaler {
void FgNoteFlip(int) {}
void GpuComplete(uint64_t, bool) {}
}
