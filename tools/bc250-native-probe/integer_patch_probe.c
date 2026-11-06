#define main upstream_probe_main
#include "probe.c"
#undef main
static bool RunIntegerLanes(Device* d, Common* c, const uint8_t* code, size_t size, Bo* extra,
                            uint32_t out[64], double timeout_sec, uint32_t vgprs, bool wave32) {
	Bo  dst;
	int r = MemCreate(d, &dst, MEM_GTT, 64 * KiB, NULL);
	if (r == 0) {
		r = BoBind(d, &dst, 0x0200000000ull, 0);
	}
	if (r != 0) {
		printf("  setup failed: %s\n", ErrStr(r));
		BoDestroy(d, &dst);
		return false;
	}
	for (uint32_t i = 0; i < 128; ++i)
		((uint32_t*)dst.cpu)[i] = 0xabc00000u + i;
	memset((uint8_t*)dst.cpu + 1024, 0xee, 64 * 4);
	LoadCode(c, code, size);
	uint64_t output_va = dst.va + 1024;
	uint32_t ud[4]     = {(uint32_t)dst.va, (uint32_t)(dst.va >> 32), (uint32_t)output_va,
	                      (uint32_t)(output_va >> 32)};
	Dispatch p         = {c->code.va, wave32 ? 32 : 64, vgprs, 4, false, 64, 1, ud};
	Ib       ib        = {.n = 0};
	IbDispatch(&ib, &p);
	IbPad(&ib);
	Bo*      bos[] = {&dst, &c->code, extra};
	uint64_t seq   = 0;
	double   t0    = NowSec();
	r              = Submit(d, &c->ib, &ib, bos, extra != NULL ? 3 : 2, &seq);
	if (r == 0) {
		r = Wait(d, seq, timeout_sec);
	}
	int64_t state = ContextState(d);
	printf("  job: %s after %.1f ms, context state 0x%" PRIx64 "\n",
	       r == 0 ? "signaled" : (r == 1 ? "TIMEOUT" : ErrStr(r)), (NowSec() - t0) * 1e3,
	       (uint64_t)state);
	memcpy(out, (uint8_t*)dst.cpu + 1024, 64 * 4);
	BoDestroy(d, &dst);
	return r == 0 && state == 0;
}

int main(int argc, char** argv) {
	if (argc != 4) return 2;
	uint8_t  code[48 * 1024];
	uint32_t expected[64], actual[64];
	FILE*    f = fopen(argv[1], "rb");
	if (!f) return 3;
	size_t size = fread(code, 1, sizeof(code), f);
	fclose(f);
	f = fopen(argv[2], "rb");
	if (!f) return 3;
	size_t count = fread(expected, sizeof(uint32_t), 64, f);
	fclose(f);
	if (!size || size % 4 || count != 64) return 4;
	Device d;
	if (DeviceOpen(&d, "/dev/dri/renderD128") != 0) return 5;
	Common c = {0};
	if (CommonInit(&d, &c) != 0) return 6;
	bool ok = RunIntegerLanes(&d, &c, code, size, NULL, actual, 5.0, 20, atoi(argv[3]) == 32);
	if (ok)
		for (int i = 0; i < 64; i++)
			if (actual[i] != expected[i]) {
				fprintf(stderr, "lane %d: got %08x expected %08x\n", i, actual[i], expected[i]);
				ok = false;
			}
	BoDestroy(&d, &c.code);
	BoDestroy(&d, &c.ib);
	close(d.fd);
	printf("Native integer shader hardware result wave%s: %s (64 lanes)\n", argv[3],
	       ok ? "PASS" : "FAIL");
	return ok ? 0 : 7;
}
