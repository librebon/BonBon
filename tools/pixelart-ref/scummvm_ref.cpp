// Reference outputs from ScummVM's scalers (AdvanceMAME Scale2x/3x row kernels, 2xSaI, Super 2xSaI,
// Super Eagle) for tests/data/pixelart. Built and run by make-refs.sh, never by the main build.
// usage: scummvm_ref <scale2x|scale3x|scale4x|2xsai|super2xsai|supereagle> in.raw out.raw
#include "common/scummsys.h"
#include "graphics/colormasks.h"
#include "graphics/scaler/scale2x.h"
#include "graphics/scaler/scale3x.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include SM_SAI_TEMPLATES

typedef Graphics::ColorMasks<8888> CM;
struct Img { int w = 0, h = 0; std::vector<uint32> px; uint32 at(int x, int y) const {
    x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return px[y * w + x]; } };

static Img load(const char *f) { Img i; FILE *fp = fopen(f, "rb"); uint32 hd[2]; fread(hd, 4, 2, fp);
    i.w = hd[0]; i.h = hd[1]; i.px.resize(i.w * i.h); fread(i.px.data(), 4, i.px.size(), fp); fclose(fp); return i; }
static void save(const char *f, const Img &i) { FILE *fp = fopen(f, "wb"); uint32 hd[2] = {uint32(i.w), uint32(i.h)};
    fwrite(hd, 4, 2, fp); fwrite(i.px.data(), 4, i.px.size(), fp); fclose(fp); }
// copy with a clamped border of `p` pixels (ScummVM scalers read outside the source rectangle)
static Img pad(const Img &s, int p) { Img o; o.w = s.w + 2 * p; o.h = s.h + 2 * p; o.px.resize(o.w * o.h);
    for (int y = 0; y < o.h; ++y) for (int x = 0; x < o.w; ++x) o.px[y * o.w + x] = s.at(x - p, y - p); return o; }

static Img adv(const Img &s, int n) {
    Img p = pad(s, 1), o; o.w = s.w * n; o.h = s.h * n; o.px.resize(o.w * o.h);
    for (int y = 0; y < s.h; ++y) {
        const uint32 *r0 = &p.px[(y) * p.w + 1], *r1 = &p.px[(y + 1) * p.w + 1], *r2 = &p.px[(y + 2) * p.w + 1];
        if (n == 2) scale2x_32_def(&o.px[(2 * y) * o.w], &o.px[(2 * y + 1) * o.w], r0, r1, r2, s.w);
        else scale3x_32_def(&o.px[(3 * y) * o.w], &o.px[(3 * y + 1) * o.w], &o.px[(3 * y + 2) * o.w], r0, r1, r2, s.w);
    }
    return o;
}

typedef void (*SaiFn)(const uint8 *, uint32, uint8 *, uint32, int, int);
static Img sai(const Img &s, SaiFn fn) {
    Img p = pad(s, 2), o; o.w = s.w * 2; o.h = s.h * 2; o.px.resize(o.w * o.h);
    fn(reinterpret_cast<const uint8 *>(&p.px[2 * p.w + 2]), p.w * 4, reinterpret_cast<uint8 *>(o.px.data()), o.w * 4, s.w, s.h);
    return o;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const std::string m = argv[1];
    Img s = load(argv[2]), o;
    if (m == "scale2x") o = adv(s, 2);
    else if (m == "scale3x") o = adv(s, 3);
    else if (m == "scale4x") o = adv(adv(s, 2), 2);
    else if (m == "2xsai") o = sai(s, _2xSaITemplate<CM>);
    else if (m == "super2xsai") o = sai(s, Super2xSaITemplate<CM>);
    else if (m == "supereagle") o = sai(s, SuperEagleTemplate<CM>);
    else return 2;
    save(argv[3], o);
    return 0;
}
