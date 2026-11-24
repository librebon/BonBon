#include "PixelScale.h"
#include "CorePrefs.h"
#include "libxbr/filters.h"
#include "xbrz/xbrz.h"
#include <QHash>
#include <QtConcurrent/QtConcurrentMap>
#include <cmath>
#include <memory>
#include <mutex>

namespace pixelscale {

// ---------------------------------------------------------------- names and factor plans
QString name(Method m) {
    switch (m) {
    case Method::Nearest: return QStringLiteral("Nearest Neighbor");
    case Method::ScaleNx: return QStringLiteral("EPX / Scale2x\u2013Scale4x");
    case Method::Eagle: return QStringLiteral("Eagle");
    case Method::SaI2x: return QStringLiteral("2xSaI");
    case Method::Super2xSaI: return QStringLiteral("Super 2xSaI");
    case Method::SuperEagle: return QStringLiteral("Super Eagle");
    case Method::Hqx: return QStringLiteral("hqx (hq2x\u2013hq4x)");
    case Method::Xbr: return QStringLiteral("xBR (2x\u20134x)");
    case Method::Xbrz: return QStringLiteral("xBRZ (2x\u20136x)");
    case Method::Mmpx: return QStringLiteral("MMPX");
    }
    return {};
}

QString key(Method m) {
    static const char *k[kMethodCount] = {"nearest", "scalenx", "eagle", "2xsai", "super2xsai", "supereagle", "hqx", "xbr", "xbrz", "mmpx"};
    return QString::fromLatin1(k[int(m)]);
}

QVector<int> nativeFactors(Method m) {
    switch (m) {
    case Method::Nearest: return {2, 3, 4, 5, 6, 7, 8};
    case Method::ScaleNx: return {2, 3};        // Scale4x is Scale2x twice (as AdvanceMAME defines it)
    case Method::Hqx: case Method::Xbr: return {2, 3, 4};
    case Method::Xbrz: return {2, 3, 4, 5, 6};
    default: return {2};
    }
}

QVector<int> passesFor(Method m, int factor) {
    if (factor <= 1) return {};
    const QVector<int> nat = nativeFactors(m);
    if (nat.contains(factor)) return {factor};
    // breadth-first over products: fewest passes for an exact product; otherwise the smallest
    // product above the factor (fewest passes among equals)
    struct Node { int prod; QVector<int> seq; };
    QHash<int, QVector<int>> best;   // product -> shortest chain
    QVector<Node> frontier{{1, {}}};
    for (int depth = 0; depth < 8 && !frontier.isEmpty(); ++depth) {
        QVector<Node> next;
        for (const Node &n : frontier)
            for (int f : nat) {
                const int p = n.prod * f;
                if (p > factor * 8 || best.contains(p)) continue;
                QVector<int> s = n.seq;
                s << f;
                best.insert(p, s);
                if (p < factor) next << Node{p, s};
            }
        if (best.contains(factor)) {
            QVector<int> s = best.value(factor);
            std::sort(s.begin(), s.end(), std::greater<int>());
            return s;
        }
        frontier = next;
    }
    int bestP = 0;
    for (auto it = best.begin(); it != best.end(); ++it)
        if (it.key() > factor && (bestP == 0 || it.key() < bestP || (it.key() == bestP && it.value().size() < best.value(bestP).size())))
            bestP = it.key();
    QVector<int> s = best.value(bestP);
    std::sort(s.begin(), s.end(), std::greater<int>());
    return s;
}

// ---------------------------------------------------------------- helpers
namespace {

// Straight-alpha source with clamped (edge-replicating) access.
struct Src {
    const QImage img;
    const int w, h;
    explicit Src(const QImage &i) : img(i), w(i.width()), h(i.height()) {}
    inline QRgb at(int x, int y) const {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
    }
};

QImage straight(const QImage &src) {
    return src.format() == QImage::Format_ARGB32 ? src : src.convertToFormat(QImage::Format_ARGB32);
}

inline QRgb *row(QImage &img, int y) { return reinterpret_cast<QRgb *>(img.scanLine(y)); }

// per-channel (alpha included) weighted means, rounded down
inline QRgb mix2(QRgb a, int wa, QRgb b, int wb, int shift) {
    QRgb out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= (((((a >> s) & 0xFF) * wa + ((b >> s) & 0xFF) * wb) >> shift) << s);
    return out;
}
inline QRgb mix3(QRgb a, int wa, QRgb b, int wb, QRgb c, int wc, int shift) {
    QRgb out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= (((((a >> s) & 0xFF) * wa + ((b >> s) & 0xFF) * wb + ((c >> s) & 0xFF) * wc) >> shift) << s);
    return out;
}
inline QRgb avg2(QRgb a, QRgb b) { return a == b ? a : mix2(a, 1, b, 1, 1); }
inline QRgb avg31(QRgb a, QRgb b) { return a == b ? a : mix2(a, 3, b, 1, 2); }
inline QRgb avg611(QRgb a, QRgb b, QRgb c) { return mix3(a, 6, b, 1, c, 1, 3); }
inline QRgb avg4(QRgb a, QRgb b, QRgb c, QRgb d) {
    QRgb out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= (((((a >> s) & 0xFF) + ((b >> s) & 0xFF) + ((c >> s) & 0xFF) + ((d >> s) & 0xFF)) >> 2) << s);
    return out;
}

// ---------------------------------------------------------------- nearest
QImage nearest(const Src &s, int n) {
    QImage out(s.w * n, s.h * n, QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y) {
        QRgb *d = row(out, y);
        const QRgb *r = reinterpret_cast<const QRgb *>(s.img.constScanLine(y / n));
        for (int x = 0; x < out.width(); ++x) d[x] = r[x / n];
    }
    return out;
}

// ---------------------------------------------------------------- EPX / Scale2x, Scale3x
//   A B C
//   D E F
//   G H I
QImage scale2x(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            const QRgb B = s.at(x, y - 1), D = s.at(x - 1, y), E = s.at(x, y), F = s.at(x + 1, y), H = s.at(x, y + 1);
            QRgb e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H) e2 = D;
                if (H == F) e3 = F;
            }
            d0[2 * x] = e0; d0[2 * x + 1] = e1; d1[2 * x] = e2; d1[2 * x + 1] = e3;
        }
    }
    return out;
}

QImage scale3x(const Src &s) {
    QImage out(s.w * 3, s.h * 3, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 3 * y), *d1 = row(out, 3 * y + 1), *d2 = row(out, 3 * y + 2);
        for (int x = 0; x < s.w; ++x) {
            const QRgb A = s.at(x - 1, y - 1), B = s.at(x, y - 1), C = s.at(x + 1, y - 1);
            const QRgb D = s.at(x - 1, y), E = s.at(x, y), F = s.at(x + 1, y);
            const QRgb G = s.at(x - 1, y + 1), H = s.at(x, y + 1), I = s.at(x + 1, y + 1);
            QRgb e[9] = {E, E, E, E, E, E, E, E, E};
            if (B != H && D != F) {
                e[0] = D == B ? D : E;
                e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                e[2] = B == F ? F : E;
                e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                e[6] = D == H ? D : E;
                e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                e[8] = H == F ? F : E;
            }
            const int o = 3 * x;
            d0[o] = e[0]; d0[o + 1] = e[1]; d0[o + 2] = e[2];
            d1[o] = e[3]; d1[o + 1] = e[4]; d1[o + 2] = e[5];
            d2[o] = e[6]; d2[o + 1] = e[7]; d2[o + 2] = e[8];
        }
    }
    return out;
}

// ---------------------------------------------------------------- Eagle
// Each output corner takes the diagonal neighbour when it and the two edge neighbours beside
// that corner agree, else the centre pixel.
QImage eagle(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            const QRgb A = s.at(x - 1, y - 1), B = s.at(x, y - 1), C = s.at(x + 1, y - 1);
            const QRgb D = s.at(x - 1, y), E = s.at(x, y), F = s.at(x + 1, y);
            const QRgb G = s.at(x - 1, y + 1), H = s.at(x, y + 1), I = s.at(x + 1, y + 1);
            d0[2 * x] = (D == A && A == B) ? A : E;
            d0[2 * x + 1] = (B == C && C == F) ? C : E;
            d1[2 * x] = (D == G && G == H) ? G : E;
            d1[2 * x + 1] = (H == I && I == F) ? I : E;
        }
    }
    return out;
}

// ---------------------------------------------------------------- 2xSaI family (Derek Liauw Kie Fa)
inline int saiResult(QRgb a, QRgb b, QRgb c, QRgb d) {
    const int x = int(a == c) + int(a == d);
    const int y = int(b == c && a != c) + int(b == d && a != d);
    return (y >> 1) - (x >> 1);
}

QImage sai2x(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            //   I|E F|J
            //   G|A B|K
            //   H|C D|L
            //   M|N O|P
            const QRgb I = s.at(x - 1, y - 1), E = s.at(x, y - 1), F = s.at(x + 1, y - 1), J = s.at(x + 2, y - 1);
            const QRgb G = s.at(x - 1, y), A = s.at(x, y), B = s.at(x + 1, y), K = s.at(x + 2, y);
            const QRgb H = s.at(x - 1, y + 1), C = s.at(x, y + 1), D = s.at(x + 1, y + 1), L = s.at(x + 2, y + 1);
            const QRgb M = s.at(x - 1, y + 2), N = s.at(x, y + 2), O = s.at(x + 1, y + 2);
            QRgb right, below, diag;
            if (A == D && B != C) {
                right = ((A == E && B == L) || (A == C && A == F && B != E && B == J)) ? A : avg2(A, B);
                below = ((A == G && C == O) || (A == B && A == H && G != C && C == M)) ? A : avg2(A, C);
                diag = A;
            } else if (B == C && A != D) {
                right = ((B == F && A == H) || (B == E && B == D && A != F && A == I)) ? B : avg2(A, B);
                below = ((C == H && A == F) || (C == G && C == D && A != H && A == I)) ? C : avg2(A, C);
                diag = B;
            } else if (A == D && B == C) {
                if (A == B) {
                    right = below = diag = A;
                } else {
                    below = avg2(A, C);
                    right = avg2(A, B);
                    int r = 0;
                    r += saiResult(A, B, G, E);
                    r -= saiResult(B, A, K, F);
                    r -= saiResult(B, A, H, N);
                    r += saiResult(A, B, L, O);
                    diag = r > 0 ? A : (r < 0 ? B : avg4(A, B, C, D));
                }
            } else {
                diag = avg4(A, B, C, D);
                if (A == C && A == F && B != E && B == J) right = A;
                else if (B == E && B == D && A != F && A == I) right = B;
                else right = avg2(A, B);
                if (A == B && A == H && G != C && C == M) below = A;
                else if (C == G && C == D && A != H && A == I) below = C;
                else below = avg2(A, C);
            }
            d0[2 * x] = A; d0[2 * x + 1] = right; d1[2 * x] = below; d1[2 * x + 1] = diag;
        }
    }
    return out;
}

QImage super2xsai(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            //  B0 B1 B2 B3
            //   4  5  6 S2
            //   1  2  3 S1
            //  A0 A1 A2 A3
            const QRgb B0 = s.at(x - 1, y - 1), B1 = s.at(x, y - 1), B2 = s.at(x + 1, y - 1), B3 = s.at(x + 2, y - 1);
            const QRgb c4 = s.at(x - 1, y), c5 = s.at(x, y), c6 = s.at(x + 1, y), S2 = s.at(x + 2, y);
            const QRgb c1 = s.at(x - 1, y + 1), c2 = s.at(x, y + 1), c3 = s.at(x + 1, y + 1), S1 = s.at(x + 2, y + 1);
            const QRgb A0 = s.at(x - 1, y + 2), A1 = s.at(x, y + 2), A2 = s.at(x + 1, y + 2), A3 = s.at(x + 2, y + 2);
            QRgb p1a, p1b, p2a, p2b;
            if (c2 == c6 && c5 != c3) {
                p2b = p1b = c2;
            } else if (c5 == c3 && c2 != c6) {
                p2b = p1b = c5;
            } else if (c5 == c3 && c2 == c6) {
                int r = 0;
                r += saiResult(c6, c5, c1, A1);
                r += saiResult(c6, c5, c4, B1);
                r += saiResult(c6, c5, A2, S1);
                r += saiResult(c6, c5, B2, S2);
                p2b = p1b = r > 0 ? c6 : (r < 0 ? c5 : avg2(c5, c6));
            } else {
                if (c6 == c3 && c3 == A1 && c2 != A2 && c3 != A0) p2b = avg31(c3, c2);
                else if (c5 == c2 && c2 == A2 && A1 != c3 && c2 != A3) p2b = avg31(c2, c3);
                else p2b = avg2(c2, c3);
                if (c6 == c3 && c6 == B1 && c5 != B2 && c6 != B0) p1b = avg31(c6, c5);
                else if (c5 == c2 && c5 == B2 && B1 != c6 && c5 != B3) p1b = avg31(c5, c6);
                else p1b = avg2(c5, c6);
            }
            if (c5 == c3 && c2 != c6 && c4 == c5 && c5 != A2) p2a = avg2(c2, c5);
            else if (c5 == c1 && c6 == c5 && c4 != c2 && c5 != A0) p2a = avg2(c2, c5);
            else p2a = c2;
            if (c2 == c6 && c5 != c3 && c1 == c2 && c2 != B2) p1a = avg2(c2, c5);
            else if (c4 == c2 && c3 == c2 && c1 != c5 && c2 != B0) p1a = avg2(c2, c5);
            else p1a = c5;
            d0[2 * x] = p1a; d0[2 * x + 1] = p1b; d1[2 * x] = p2a; d1[2 * x + 1] = p2b;
        }
    }
    return out;
}

QImage supereagle(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            const QRgb B1 = s.at(x, y - 1), B2 = s.at(x + 1, y - 1);
            const QRgb c4 = s.at(x - 1, y), c5 = s.at(x, y), c6 = s.at(x + 1, y), S2 = s.at(x + 2, y);
            const QRgb c1 = s.at(x - 1, y + 1), c2 = s.at(x, y + 1), c3 = s.at(x + 1, y + 1), S1 = s.at(x + 2, y + 1);
            const QRgb A1 = s.at(x, y + 2), A2 = s.at(x + 1, y + 2);
            QRgb p1a, p1b, p2a, p2b;
            if (c5 != c3) {
                if (c2 == c6) {
                    p1b = p2a = c2;
                    p1a = (c1 == c2 || c6 == B2) ? avg31(c2, c5) : avg2(c5, c6);
                    p2b = (c6 == S2 || c2 == A1) ? avg31(c2, c3) : avg2(c2, c3);
                } else {
                    p2b = avg611(c3, c2, c6);
                    p1a = avg611(c5, c2, c6);
                    p2a = avg611(c2, c5, c3);
                    p1b = avg611(c6, c5, c3);
                }
            } else if (c2 != c6) {
                p2b = p1a = c5;
                p1b = (B1 == c5 || c3 == S1) ? avg31(c5, c6) : avg2(c5, c6);
                p2a = (c3 == A2 || c4 == c5) ? avg31(c5, c2) : avg2(c2, c3);
            } else {
                int r = 0;
                r += saiResult(c6, c5, c1, A1);
                r += saiResult(c6, c5, c4, B1);
                r += saiResult(c6, c5, A2, S1);
                r += saiResult(c6, c5, B2, S2);
                if (r > 0) {
                    p1b = p2a = c2;
                    p1a = p2b = avg2(c5, c6);
                } else if (r < 0) {
                    p2b = p1a = c5;
                    p1b = p2a = avg2(c5, c6);
                } else {
                    p2b = p1a = c5;
                    p1b = p2a = c2;
                }
            }
            d0[2 * x] = p1a; d0[2 * x + 1] = p1b; d1[2 * x] = p2a; d1[2 * x + 1] = p2b;
        }
    }
    return out;
}

// ---------------------------------------------------------------- MMPX (McGuire & Gagiu 2021)
inline quint32 mmpxLuma(QRgb c) {
    return (((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF) + 1) * (256 - (c >> 24));
}
inline bool allEq2(QRgb b, QRgb a0, QRgb a1) { return b == a0 && b == a1; }
inline bool allEq3(QRgb b, QRgb a0, QRgb a1, QRgb a2) { return b == a0 && b == a1 && b == a2; }
inline bool allEq4(QRgb b, QRgb a0, QRgb a1, QRgb a2, QRgb a3) { return b == a0 && b == a1 && b == a2 && b == a3; }
inline bool anyEq3(QRgb b, QRgb a0, QRgb a1, QRgb a2) { return b == a0 || b == a1 || b == a2; }
inline bool noneEq2(QRgb b, QRgb a0, QRgb a1) { return b != a0 && b != a1; }
inline bool noneEq4(QRgb b, QRgb a0, QRgb a1, QRgb a2, QRgb a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

QImage mmpx(const Src &s) {
    QImage out(s.w * 2, s.h * 2, QImage::Format_ARGB32);
    for (int y = 0; y < s.h; ++y) {
        QRgb *d0 = row(out, 2 * y), *d1 = row(out, 2 * y + 1);
        for (int x = 0; x < s.w; ++x) {
            auto src = [&](int dx, int dy) { return s.at(x + dx, y + dy); };
            const QRgb A = src(-1, -1), B = src(0, -1), C = src(1, -1);
            const QRgb D = src(-1, 0), E = src(0, 0), F = src(1, 0);
            const QRgb G = src(-1, 1), H = src(0, 1), I = src(1, 1);
            QRgb J = E, K = E, L = E, M = E;
            if (A != E || B != E || C != E || D != E || F != E || G != E || H != E || I != E) {
                const QRgb P = src(0, -2), S = src(0, 2), Q = src(-2, 0), R = src(2, 0);
                const quint32 Bl = mmpxLuma(B), Dl = mmpxLuma(D), El = mmpxLuma(E), Fl = mmpxLuma(F), Hl = mmpxLuma(H);
                // 1:1 slope rules
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && anyEq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && anyEq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && anyEq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && anyEq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;
                // intersection rules
                if ((E != F && allEq4(E, C, I, D, Q) && allEq2(F, B, H)) && (F != src(3, 0))) K = M = F;
                if ((E != D && allEq4(E, A, G, F, R) && allEq2(D, B, H)) && (D != src(-3, 0))) J = L = D;
                if ((E != H && allEq4(E, G, I, B, P) && allEq2(H, D, F)) && (H != src(0, 3))) L = M = H;
                if ((E != B && allEq4(E, A, C, H, S) && allEq2(B, D, F)) && (B != src(0, -3))) J = K = B;
                if (Bl < El && allEq4(E, G, H, I, S) && noneEq4(E, A, D, C, F)) J = K = B;
                if (Hl < El && allEq4(E, A, B, C, P) && noneEq4(E, D, G, I, F)) L = M = H;
                if (Fl < El && allEq4(E, A, D, G, Q) && noneEq4(E, B, C, I, H)) K = M = F;
                if (Dl < El && allEq4(E, C, F, I, R) && noneEq4(E, B, A, G, H)) J = L = D;
                // 2:1 slope rules
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (allEq3(H, G, F, R) && noneEq2(H, D, src(2, -1))) L = M;
                        if (allEq3(H, I, D, Q) && noneEq2(H, F, src(-2, -1))) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (allEq3(B, A, F, R) && noneEq2(B, D, src(2, 1))) J = K;
                        if (allEq3(B, C, D, Q) && noneEq2(B, F, src(-2, 1))) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (allEq3(D, A, H, S) && noneEq2(D, B, src(1, 2))) J = L;
                        if (allEq3(D, G, B, P) && noneEq2(D, H, src(1, -2))) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (allEq3(F, C, H, S) && noneEq2(F, B, src(-1, 2))) K = M;
                        if (allEq3(F, I, B, P) && noneEq2(F, H, src(-1, -2))) M = K;
                    }
                }
            }
            d0[2 * x] = J; d0[2 * x + 1] = K; d1[2 * x] = L; d1[2 * x + 1] = M;
        }
    }
    return out;
}

// ---------------------------------------------------------------- hqx / xBR (libxbr) and xBRZ
const xbr_data *xbrData() {
    // the RGB -> YUV table is 64 MB; built once, the first time hqx or xBR runs
    static std::unique_ptr<xbr_data> data;
    static std::once_flag once;
    std::call_once(once, [] { data.reset(new xbr_data); xbr_init_data(data.get()); });
    return data.get();
}

QImage libxbr(const QImage &src, Method m, int n) {
    QImage out(src.width() * n, src.height() * n, QImage::Format_ARGB32);
    xbr_params p;
    p.input = src.constBits();
    p.output = out.bits();
    p.inWidth = src.width();
    p.inHeight = src.height();
    p.inPitch = int(src.bytesPerLine());
    p.outPitch = int(out.bytesPerLine());
    p.data = xbrData();
    if (m == Method::Hqx) {
        if (n == 2) xbr_filter_hq2x(&p); else if (n == 3) xbr_filter_hq3x(&p); else xbr_filter_hq4x(&p);
    } else {
        if (n == 2) xbr_filter_xbr2x(&p); else if (n == 3) xbr_filter_xbr3x(&p); else xbr_filter_xbr4x(&p);
    }
    return out;
}

QImage xbrzScale(const QImage &src, int n) {
    // xBRZ's ARGB mode reads outside the image as transparent, which fades the corners of an
    // opaque bitmap. Every other scaler here clamps to the edge, so pad by duplicating the border
    // (the kernel reaches 2 px out) and crop the result back.
    constexpr int P = 2;
    const int w = src.width(), h = src.height(), pw = w + 2 * P, ph = h + 2 * P;
    QVector<quint32> in(size_t(pw) * ph);
    for (int y = 0; y < ph; ++y) {
        const quint32 *srcRow = reinterpret_cast<const quint32 *>(src.constScanLine(qBound(0, y - P, h - 1)));
        for (int x = 0; x < pw; ++x) in[y * pw + x] = srcRow[qBound(0, x - P, w - 1)];
    }
    QVector<quint32> o(in.size() * n * n);
    xbrz::scale(size_t(n), in.constData(), o.data(), pw, ph, xbrz::ColorFormat::ARGB);
    QImage out(w * n, h * n, QImage::Format_ARGB32);
    const int ow = pw * n;
    for (int y = 0; y < out.height(); ++y)
        memcpy(out.scanLine(y), o.constData() + size_t(y + P * n) * ow + P * n, size_t(out.width()) * 4);
    return out;
}

} // namespace

// ---------------------------------------------------------------- entry points
QImage pass(const QImage &srcIn, Method m, int n) {
    if (srcIn.isNull()) return {};
    const QImage src = straight(srcIn);
    if (n <= 1) return src;
    if (!nativeFactors(m).contains(n)) return upscale(src, m, n);
    const Src s(src);
    switch (m) {
    case Method::Nearest: return nearest(s, n);
    case Method::ScaleNx: return n == 3 ? scale3x(s) : scale2x(s);
    case Method::Eagle: return eagle(s);
    case Method::SaI2x: return sai2x(s);
    case Method::Super2xSaI: return super2xsai(s);
    case Method::SuperEagle: return supereagle(s);
    case Method::Hqx: case Method::Xbr: return libxbr(src, m, n);
    case Method::Xbrz: return xbrzScale(src, n);
    case Method::Mmpx: return mmpx(s);
    }
    return src;
}


// ---------------------------------------------------------------- filtered resampling (v0.3.4)
namespace {
double cubicBC(double x, double B, double C) {
    x = std::fabs(x);
    if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6.0;
    if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6.0;
    return 0;
}
double kernelAt(Filter f, double x) {
    switch (f) {
    case Filter::Bicubic: return cubicBC(x, 0.0, 0.5);    // Catmull-Rom: sharp, the usual "bicubic"
    case Filter::Soft: return cubicBC(x, 1.0, 0.0);       // cubic B-spline: smooth, no ringing
    case Filter::Bilinear: default: x = std::fabs(x); return x < 1 ? 1 - x : 0;
    }
}
double supportOf(Filter f) { return f == Filter::Bilinear ? 1.0 : 2.0; }

struct Taps { int first = 0; QVector<float> w; };

// One output column's (or row's) source taps: centre-aligned ((d + 0.5) / scale - 0.5), the kernel
// widened by 1 / scale when shrinking (so every source pixel contributes), indices clamped to the
// edge (pixels outside the image are copies of the edge, never transparent or white), weights
// normalised to sum to 1.
QVector<Taps> tapsFor(int srcN, int dstN, Filter f) {
    const double scale = double(dstN) / srcN;
    const double stretch = scale < 1 ? 1 / scale : 1;
    const double support = supportOf(f) * stretch;
    QVector<Taps> out(dstN);
    for (int d = 0; d < dstN; ++d) {
        const double c = (d + 0.5) / scale - 0.5;
        const int lo = int(std::floor(c - support)) + 1, hi = int(std::floor(c + support));
        Taps &t = out[d];
        t.first = lo;
        t.w.resize(hi - lo + 1);
        double sum = 0;
        for (int i = lo; i <= hi; ++i) sum += (t.w[i - lo] = float(kernelAt(f, (i - c) / stretch)));
        if (std::fabs(sum) < 1e-12) { t.w.fill(0); t.w[qBound(0, int(std::lround(c)) - lo, int(t.w.size()) - 1)] = 1; sum = 1; }
        for (float &w : t.w) w = float(w / sum);
    }
    return out;
}

template <typename Fn> void parallelRows(int n, Fn fn) {
    if (n < 64) { for (int i = 0; i < n; ++i) fn(i); return; }
    QVector<int> chunks;
    for (int i = 0; i < n; i += 32) chunks << i;
    QtConcurrent::blockingMap(chunks, [&](int start) { for (int i = start; i < qMin(n, start + 32); ++i) fn(i); });
}

inline uchar clampByte(float v, float hi) { return uchar(qBound(0.0f, std::round(v), hi)); }
}

QImage filterResample(const QImage &srcIn, const QSize &size, Filter f) {
    if (srcIn.isNull() || size.isEmpty()) return {};
    const QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int sw = src.width(), sh = src.height(), dw = size.width(), dh = size.height();
    // horizontal pass: sh rows of dw premultiplied float pixels
    const QVector<Taps> tx = tapsFor(sw, dw, f), ty = tapsFor(sh, dh, f);
    std::vector<float> mid(size_t(sh) * dw * 4);
    parallelRows(sh, [&](int y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        float *o = mid.data() + size_t(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const Taps &t = tx[x];
            float a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < t.w.size(); ++k) {
                const QRgb px = row[qBound(0, t.first + k, sw - 1)];
                const float w = t.w[k];
                a += w * qAlpha(px); r += w * qRed(px); g += w * qGreen(px); b += w * qBlue(px);
            }
            o[x * 4] = a; o[x * 4 + 1] = r; o[x * 4 + 2] = g; o[x * 4 + 3] = b;
        }
    });
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    parallelRows(dh, [&](int y) {
        const Taps &t = ty[y];
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < dw; ++x) {
            float a = 0, r = 0, g = 0, b = 0;
            for (int k = 0; k < t.w.size(); ++k) {
                const float *m = mid.data() + (size_t(qBound(0, t.first + k, sh - 1)) * dw + x) * 4;
                const float w = t.w[k];
                a += w * m[0]; r += w * m[1]; g += w * m[2]; b += w * m[3];
            }
            // negative lobes can over / undershoot: clamp alpha to 0..255 and colour to 0..alpha
            const uchar A = clampByte(a, 255);
            d[x] = qRgba(clampByte(r, A), clampByte(g, A), clampByte(b, A), A);
        }
    });
    return srcIn.hasAlphaChannel() || srcIn.format() == QImage::Format_ARGB32_Premultiplied
        ? (srcIn.format() == QImage::Format_ARGB32 ? out.convertToFormat(QImage::Format_ARGB32) : out)
        : out.convertToFormat(srcIn.format());
}

static QImage downTo(const QImage &big, const QSize &size, QImage::Format fmt) {
    if (big.size() == size) return big.convertToFormat(fmt);
    // area-weighted shrink in premultiplied space (so transparent pixels do not bleed colour)
    // (edge-clamped, centre-aligned: no fringe along the image edges)
    return filterResample(big, size, Filter::Bilinear).convertToFormat(fmt);
}

QImage upscale(const QImage &src, Method m, int factor) {
    if (src.isNull()) return {};
    if (factor <= 1) return src;
    QImage cur = straight(src);
    for (int f : passesFor(m, factor)) cur = pass(cur, m, f);
    return downTo(cur, src.size() * factor, src.format() == QImage::Format_ARGB32 ? QImage::Format_ARGB32 : src.format());
}

QImage scaleTo(const QImage &src, const QSize &sizeIn, Method m) {
    if (src.isNull() || sizeIn.isEmpty()) return {};
    const QSize size(qMax(1, sizeIn.width()), qMax(1, sizeIn.height()));
    if (size == src.size()) return src;
    const QImage::Format fmt = src.hasAlphaChannel() ? src.format() : QImage::Format_ARGB32;
    if (size.width() <= src.width() && size.height() <= src.height())
        return src.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    const int fx = (size.width() + src.width() - 1) / src.width();
    const int fy = (size.height() + src.height() - 1) / src.height();
    const int f = qMax(2, qMax(fx, fy));
    QImage cur = straight(src);
    for (int p : passesFor(m, f)) cur = pass(cur, m, p);
    return downTo(cur, size, fmt);
}

// ---------------------------------------------------------------- the Resample list
QStringList resampleNames() {
    QStringList l{QStringLiteral("Bicubic"), QStringLiteral("Bilinear"), QStringLiteral("Soft"), QStringLiteral("Nearest Neighbor")};
    for (int i = 1; i < kMethodCount; ++i) l << QStringLiteral("Pixel art: ") + name(Method(i));
    return l;
}

QString resampleKey(int r) {
    static const char *base[4] = {"bicubic", "bilinear", "soft", "nearest"};
    if (r >= 0 && r < kFirstPixelArt) return QString::fromLatin1(base[r]);
    if (r >= kFirstPixelArt && r < kResampleCount) return key(Method(r - kFirstPixelArt + 1));
    return QStringLiteral("default");
}

int resampleFromKey(const QString &k, int fallback) {
    for (int r = 0; r < kResampleCount; ++r)
        if (resampleKey(r) == k) return r;
    return fallback;
}

bool isPixelArt(int r) { return r >= kFirstPixelArt && r < kResampleCount; }
Method methodFor(int r) { return isPixelArt(r) ? Method(r - kFirstPixelArt + 1) : Method::Nearest; }
int resampleFor(Method m) { return m == Method::Nearest ? int(coreprefs::Nearest) : kFirstPixelArt + int(m) - 1; }

QImage resample(const QImage &src, const QSize &size, int r) {
    if (r < 0) r = coreprefs::interpolation;
    if (src.isNull() || size == src.size()) return src;
    if (isPixelArt(r)) return scaleTo(src, size, methodFor(r));
    if (r == coreprefs::Nearest) return src.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    return filterResample(src, size, r == coreprefs::Bilinear ? Filter::Bilinear : r == coreprefs::Soft ? Filter::Soft : Filter::Bicubic);
}

} // namespace pixelscale
