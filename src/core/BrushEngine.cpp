#include "BrushEngine.h"
#include "Fills.h"
#include "Stroke.h"
#include "Retouch.h"
#include <QPainter>
#include <QStringList>
#include <QtMath>
#include <cmath>
#include <cstring>

namespace brush {

QString categoryName(Category c) {
    switch (c) {
    case Category::Basic: return QStringLiteral("Basic");
    case Category::AirBrush: return QStringLiteral("Air Brush");
    case Category::Calligraphy: return QStringLiteral("Calligraphy");
    case Category::Charcoal: return QStringLiteral("Charcoal");
    case Category::Crayon: return QStringLiteral("Crayon");
    case Category::FeltTip: return QStringLiteral("Felt Tip");
    case Category::Watercolor: return QStringLiteral("Watercolor");
    case Category::Pencil: return QStringLiteral("Pencil");
    case Category::Oil: return QStringLiteral("Oil");
    case Category::Dashed: return QStringLiteral("Dashed");
    case Category::Random: return QStringLiteral("Random");
    case Category::Unnatural: return QStringLiteral("Unnatural");
    case Category::Libreworks: return QStringLiteral("Libreworks");
    }
    return QStringLiteral("Basic");
}
Category categoryFromName(const QString &name) {
    // Accept both the Fireworks menu order and the stable enum names.
    for (int i = 0; i <= int(Category::Libreworks); ++i)
        if (categoryName(Category(i)) == name) return Category(i);
    return Category::Basic;
}
QStringList categoryNames() {   // stable enum order (AppState brushCategory index)
    QStringList l;
    for (int i = 0; i <= int(Category::Libreworks); ++i) l << categoryName(Category(i));
    return l;
}
QStringList tipNames(Category c) {
    const QStringList n = stroke::namesInCategory(categoryName(c));
    return n.isEmpty() ? QStringList{QStringLiteral("Basic")} : n;
}
int tipIndex(Category c, const QString &tip) {
    const int i = tipNames(c).indexOf(tip);
    return i < 0 ? 0 : i;
}
void applyTip(Params &p, Category c, int tip) {
    p.category = c;
    const QStringList names = tipNames(c);
    tip = qBound(0, tip, names.size() - 1);
    const QString key = stroke::keyFor(categoryName(c), names.value(tip));
    const stroke::Params sp = stroke::paramsFor(key);
    p.strokeKey = key;
    p.square = sp.square || (c == Category::Basic && names.value(tip).contains(QLatin1String("Hard Rounded")));
    p.edge = sp.edge;
    p.texture = sp.texture;
    p.textureAmount = sp.textureAmount;
    p.opacity = 100;
    if (sp.look == stroke::Look::Highlighter) p.opacity = 40;
    else if (names.value(tip) == QLatin1String("Light Marker")) p.opacity = 70;
    p.nibRatio = (c == Category::Calligraphy && sp.aspect > 0) ? sp.aspect : 0.25;
    p.nibAngle = sp.angle;
}

QString tipStrokeKey(Category c, int tip)
{
    Params p;
    applyTip(p, c, tip);
    return stroke::canonicalKey(p.strokeKey);
}

stroke::Params liveEditStrokeParams(const Params &bp)
{
    const QString key = stroke::canonicalKey(bp.strokeKey);
    stroke::Params p = stroke::paramsFor(key);
    p.key = key;
    p.size = bp.size;
    p.edge = bp.edge;
    p.square = bp.square;
    p.texture = bp.texture;
    p.textureAmount = bp.textureAmount;
    p.angle = bp.nibAngle;
    p.aspect = (bp.category == Category::Calligraphy && bp.nibRatio > 0) ? bp.nibRatio : 1.0;
    // The vector preset's ink/flow/build-up describe stroke::paint, not the dab engine.
    // Soft Line (ink 50) and Air Brush (flow 85, build-up) would otherwise change on OK.
    if (!stroke::usesPathEngine(p.look)) {
        p.inkAmount = 100;
        p.flowRate = 0;
        p.buildUp = false;
    }
    return p;
}



namespace {
inline float hashf(int x, int y, int seed) {
    quint32 h = quint32(x) * 374761393u + quint32(y) * 668265263u + quint32(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.f;
}
QImage texFor(const QString &name) {
    return name.isEmpty() ? QImage() : fills::textureTile(name);
}
inline double texFactor(const QImage &tex, int amount, int x, int y) {
    if (tex.isNull() || amount <= 0) return 1;
    const int tw = tex.width(), th = tex.height();
    const int tx = ((x % tw) + tw) % tw, ty = ((y % th) + th) % th;
    return 1.0 - amount / 100.0 * (1.0 - tex.constScanLine(ty)[tx] / 255.0);
}
}

void Stroke::begin(const QImage &target, const Params &p) {
    m_base = target.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_w = m_base.width();
    m_h = m_base.height();
    m_capW = m_w;
    m_capH = m_h;
    m_mask = QVector<float>(m_w * m_h, 0.f);
    m_p = p;
    // Ink, spacing, flow, aspect and sensitivity come from strokeParams when custom.
    // Size, edge, shape and texture stay on the Params the PI already filled in, so
    // those controls keep working after Edit Stroke.
    if (m_p.strokeCustom) {
        const stroke::Params &sp = m_p.strokeParams;
        m_p.nibAngle = sp.angle;
        if (sp.aspect > 0.0 && sp.aspect < 0.999)
            m_p.nibRatio = qBound(0.05, sp.aspect, 1.0);
    }
    m_p.size = qMax(p.pencil ? 1.0 : 0.5, m_p.size);
    m_tex = texFor(m_p.texture);
    m_clip = QImage();
    m_first = true;
    m_left = 0;
    m_useEngine = !m_p.pencil && !m_p.strokeKey.isEmpty()
                  && stroke::usesPathEngine(stroke::paramsFor(m_p.strokeKey).look);
    m_enginePath = QPainterPath();
    m_enginePaint = QImage();
    m_engineDirty = QRect();
    m_engineSeed = quint32(qHash(m_p.strokeKey) * 2654435761u);
    m_engineSpeed = 1;
    m_enginePressure = 1;
    m_engineDist.clear();
    m_enginePressures.clear();
    m_engineSpeeds.clear();
    m_engineTimes.clear();
    m_engineClock.start();
    m_enginePathLen = 0;
    m_engineFinalized = false;
    m_enginePathBounds = QRectF();
    m_cellCs = 0;
    m_engineDeferred = false;
    m_enginePending = false;
    m_engineLastWasFull = false;
    m_previewSense = false;
    m_engineHasLast = false;
    m_engineLast = QPointF();
    m_enginePaintedN = 0;
    m_ckpts.clear();
}

void Stroke::end() {
    m_base = QImage();
    m_clip = QImage();
    m_mask.clear();
    m_w = m_h = 0;
    m_enginePath = QPainterPath();
    m_enginePaint = QImage();
    m_engineDirty = QRect();
    m_useEngine = false;
    m_enginePending = false;
    m_engineLastWasFull = false;
    m_engineHasLast = false;
    m_engineLast = QPointF();
    m_ckpts.clear();
    stroke::releaseUnderSampleCache();
}

void Stroke::reserveCapacity(QSize cap)
{
    if (!active()) return;
    const int capW = qMax(m_capW, ((qMax(qMax(1, cap.width()), m_w) + 255) / 256) * 256);
    const int capH = qMax(m_capH, ((qMax(qMax(1, cap.height()), m_h) + 255) / 256) * 256);
    if (m_base.width() >= capW && m_base.height() >= capH
        && m_mask.capacity() >= capW * capH) {
        m_capW = qMax(m_capW, capW);
        m_capH = qMax(m_capH, capH);
        return;
    }
    if (m_base.width() < capW || m_base.height() < capH
        || m_base.format() != QImage::Format_ARGB32_Premultiplied) {
        QImage nb(capW, capH, QImage::Format_ARGB32_Premultiplied);
        nb.fill(Qt::transparent);
        if (!m_base.isNull()) {
            QPainter p(&nb);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(0, 0, m_base);
        }
        m_base = nb;
    }
    if (m_mask.capacity() < capW * capH)
        m_mask.reserve(capW * capH);
    if (!m_enginePaint.isNull()
        && (m_enginePaint.width() < capW || m_enginePaint.height() < capH)) {
        QImage np(capW, capH, QImage::Format_ARGB32_Premultiplied);
        np.fill(Qt::transparent);
        QPainter gp(&np);
        gp.setCompositionMode(QPainter::CompositionMode_Source);
        gp.drawImage(0, 0, m_enginePaint);
        m_enginePaint = np;
    }
    m_capW = capW;
    m_capH = capH;
}

void Stroke::grow(QPoint shift, QSize size) {
    if (!active()) return;
    const int nw = size.width(), nh = size.height();
    auto translateEngine = [&]() {
        if (!m_useEngine) return;
        if (!m_enginePath.isEmpty()) m_enginePath.translate(shift.x(), shift.y());
        m_engineDirty.translate(shift);
        m_engineLastDirty.translate(shift);
        if (m_engineHasLast)
            m_engineLast += QPointF(shift);
        if (!m_enginePathBounds.isEmpty())
            m_enginePathBounds.translate(shift.x(), shift.y());
        for (EngineCkpt &ck : m_ckpts)
            ck.rect.translate(shift);
    };
    auto copyMask = [&](QVector<float> &nm, int dw, int dh) {
        nm = QVector<float>(dw * dh, 0.f);
        const int x0 = qMax(0, -shift.x());
        const int n = qMin(m_w - x0, dw - (x0 + shift.x()));
        if (n <= 0) return;
        for (int y = 0; y < m_h; ++y) {
            const int ny = y + shift.y();
            if (ny < 0 || ny >= dh) continue;
            memcpy(nm.data() + ny * dw + (x0 + shift.x()),
                   m_mask.constData() + y * m_w + x0, size_t(n) * sizeof(float));
        }
    };
    const bool fits = shift.x() >= 0 && shift.y() >= 0
        && (shift.x() + m_w) <= m_capW && (shift.y() + m_h) <= m_capH
        && nw <= m_capW && nh <= m_capH
        && !m_base.isNull() && m_base.width() >= m_capW && m_base.height() >= m_capH;
    if (fits) {
        // Expand inside the existing buffers. Right/bottom growth is a fill of
        // the new strips; a left/up shift memmoves the old pixels.
        auto shiftImg = [&](QImage &im) {
            if (im.isNull() || (shift.isNull() && nw >= m_w && nh >= m_h)) {
                if (!shift.isNull()) {
                    // unreachable when im is null
                }
            }
            if (im.isNull()) return;
            if (!shift.isNull()) {
                QImage tmp = im.copy(0, 0, m_w, m_h);
                QPainter p(&im);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                if (shift.x() > 0) p.fillRect(QRect(0, 0, shift.x(), nh), Qt::transparent);
                if (shift.y() > 0) p.fillRect(QRect(0, 0, nw, shift.y()), Qt::transparent);
                p.drawImage(shift, tmp);
                // clear a right/bottom leftover strip if the used size grew
                if (shift.x() + m_w < nw)
                    p.fillRect(QRect(shift.x() + m_w, shift.y(), nw - (shift.x() + m_w), nh), Qt::transparent);
                if (shift.y() + m_h < nh)
                    p.fillRect(QRect(shift.x(), shift.y() + m_h, nw, nh - (shift.y() + m_h)), Qt::transparent);
            } else {
                QPainter p(&im);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                if (nw > m_w) p.fillRect(QRect(m_w, 0, nw - m_w, nh), Qt::transparent);
                if (nh > m_h) p.fillRect(QRect(0, m_h, nw, nh - m_h), Qt::transparent);
            }
        };
        shiftImg(m_base);
        if (!m_enginePaint.isNull()) shiftImg(m_enginePaint);
        // Keep the dab mask cheap: height-only growth is a prefix-preserving
        // resize (zeroes the new rows). A full packed rebuild is only needed
        // when the width changes or the origin shifts.
        if (shift.isNull() && nw >= m_w && nh >= m_h) {
            if (nw == m_w) {
                if (m_mask.capacity() < nw * nh)
                    m_mask.reserve(qMax(nw * nh, m_capW * m_capH));
                m_mask.resize(nw * nh);
            } else {
                QVector<float> nm;
                nm.reserve(qMax(nw * nh, m_capW * m_capH));
                nm.resize(nw * nh);
                nm.fill(0.f);
                for (int y = 0; y < m_h; ++y)
                    memcpy(nm.data() + y * nw, m_mask.constData() + y * m_w,
                           size_t(m_w) * sizeof(float));
                m_mask = std::move(nm);
            }
        } else {
            QVector<float> nm;
            copyMask(nm, nw, nh);
            m_mask = std::move(nm);
        }
        m_w = nw;
        m_h = nh;
        m_clip = QImage();
        translateEngine();
        return;
    }
    const int capW = qMax(nw, ((nw + 255) / 256) * 256);
    const int capH = qMax(nh, ((nh + 255) / 256) * 256);
    QImage nb(capW, capH, QImage::Format_ARGB32_Premultiplied);
    nb.fill(Qt::transparent);
    {
        QPainter p(&nb);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(shift, m_base);
    }
    QVector<float> nm;
    copyMask(nm, nw, nh);
    if (nm.capacity() < capW * capH)
        nm.reserve(capW * capH);
    m_base = nb;
    m_mask = std::move(nm);
    m_w = nw;
    m_h = nh;
    m_capW = capW;
    m_capH = capH;
    m_clip = QImage();
    if (m_useEngine) {
        if (!m_enginePaint.isNull()) {
            QImage np(capW, capH, QImage::Format_ARGB32_Premultiplied);
            np.fill(Qt::transparent);
            QPainter gp(&np);
            gp.setCompositionMode(QPainter::CompositionMode_Source);
            gp.drawImage(shift, m_enginePaint);
            m_enginePaint = np;
        }
        translateEngine();
    }
}

double Stroke::paintRadius() const
{
    const double margin = 2.0;
    if (m_p.pencil) return m_p.size / 2.0 + margin;
    const stroke::Params sp = m_p.strokeCustom ? m_p.strokeParams
        : (m_p.strokeKey.isEmpty() ? stroke::Params{} : stroke::paramsFor(m_p.strokeKey));
    if (m_useEngine || !m_p.strokeKey.isEmpty() || m_p.strokeCustom)
        return stroke::paintedRadius(sp, m_p.size) + margin;
    return m_p.size / 2.0 + margin;
}

QRectF Stroke::paintedBounds(QPointF a, QPointF b) const
{
    const double r = paintRadius();
    QRectF seg = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    if (m_useEngine) {
        const stroke::Params sp = m_p.strokeCustom ? m_p.strokeParams : stroke::paramsFor(m_p.strokeKey);
        const double rr = stroke::paintedRadius(sp, m_p.size);
        QRectF box = m_enginePathBounds;
        if (box.isNull()) box = QRectF(a, a);
        box = box.united(QRectF(a, b).normalized()).adjusted(-rr, -rr, rr, rr);
        if (sp.look == stroke::Look::Smoke) {
            const double len = m_enginePathLen + QLineF(a, b).length();
            box.adjust(0, -0.16 * len, 0, 0);
        }
        return box.adjusted(-2, -2, 2, 2).united(seg);
    }
    return seg;
}

QRectF Stroke::pendingPaintedBounds() const
{
    if (!active()) return {};
    const double r = paintRadius();
    QRectF box = m_enginePathBounds;
    if (box.isEmpty()) {
        if (m_engineHasLast) box = QRectF(m_engineLast, m_engineLast);
        else return {};
    }
    box = box.adjusted(-r, -r, r, r);
    if (m_useEngine) {
        const stroke::Params sp = m_p.strokeCustom ? m_p.strokeParams
            : (m_p.strokeKey.isEmpty() ? stroke::Params{} : stroke::paramsFor(m_p.strokeKey));
        if (sp.look == stroke::Look::Smoke)
            box.adjust(0, -0.16 * m_enginePathLen, 0, 0);
    }
    return box.adjusted(-2, -2, 2, 2);
}

double Stroke::sizeAt(const tablet::Sample &s) const {
    if (!(m_p.pressure & PressureSize)) return m_p.size;
    return qMax(m_p.pencil ? 1.0 : 0.5, m_p.size * tablet::withFloor(qBound(0.0, s.pressure, 1.0), qBound(0.0, m_p.minSize, 1.0)));
}

double Stroke::dabStep(const tablet::Sample &s) const
{
    const double k = m_p.category == Category::AirBrush ? 0.08 : 0.12;
    double step = sizeAt(s) * k;
    // Scale the factory dab pitch by (spacing / factory spacing) so 100 and 99
    // stay continuous, and a factory-valued OK is a no-op.
    if (m_p.strokeCustom) {
        const QString key = m_p.strokeParams.key.isEmpty() ? m_p.strokeKey : m_p.strokeParams.key;
        const int factorySp = qMax(1, stroke::paramsFor(key).spacing);
        step = sizeAt(s) * k * (double(qMax(0, m_p.strokeParams.spacing)) / factorySp);
    }
    return qMax(0.5, step);
}

void Stroke::dab(QPointF c, QRect *dirty, const tablet::Sample &s) {
    double r = sizeAt(s) / 2.0;
    if (m_p.strokeCustom && m_p.strokeParams.sizeSens.random > 0) {
        const float h = hashf(int(std::lround(c.x() * 10)), int(std::lround(c.y() * 10)), 19);
        r *= 1.0 + (h - 0.5) * (m_p.strokeParams.sizeSens.random / 50.0);
        r = qMax(0.25, r);
    }
    const double pr = qBound(0.0, s.pressure, 1.0), floorO = qBound(0.0, m_p.minOpacity, 1.0);
    const double opK = (m_p.pressure & PressureOpacity) ? tablet::withFloor(pr, floorO) : 1.0;
    const bool flow = m_p.pressure & PressureFlow;
    const double flowK = flow ? tablet::withFloor(pr, floorO) : 1.0;
    const double inkK = (m_p.strokeCustom ? qBound(0, m_p.strokeParams.inkAmount, 100) : 100) / 100.0;
    const double customFlow = (m_p.strokeCustom && m_p.strokeParams.flowRate > 0)
        ? m_p.strokeParams.flowRate / 100.0 : 0.0;
    const bool customBuild = m_p.strokeCustom && m_p.strokeParams.buildUp;
    const Category cat = m_p.category;
    int edge = m_p.edge;
    if (cat == Category::AirBrush) edge = qMax(edge, 85);
    if (cat == Category::Watercolor) edge = qMax(edge, 70);
    if (cat == Category::Charcoal) edge = qMax(edge, 30);
    const bool flat = (cat == Category::Calligraphy)
        || (m_p.strokeCustom && m_p.strokeParams.aspect > 0.0 && m_p.strokeParams.aspect < 0.999);
    const double reach = flat ? r : r + 1;
    const QRect R = QRect(QPoint(qFloor(c.x() - reach), qFloor(c.y() - reach)), QPoint(qCeil(c.x() + reach), qCeil(c.y() + reach)))
                        .intersected(QRect(0, 0, m_w, m_h));
    if (R.isEmpty()) return;
    *dirty |= R;
    const double defaultNib = m_p.nibAngle * M_PI / 180.0;
    const double nib = m_p.tilt ? tablet::nibAngle(s, defaultNib) : defaultNib;
    const double ca = std::cos(nib), sa = std::sin(nib);
    for (int y = R.top(); y <= R.bottom(); ++y) {
        float *m = m_mask.data() + y * m_w;
        for (int x = R.left(); x <= R.right(); ++x) {
            double dx = x + 0.5 - c.x(), dy = y + 0.5 - c.y();
            double d;
            if (flat) {   // Calligraphy, or Edit Stroke aspect < 1
                const double u = dx * ca - dy * sa, v = dx * sa + dy * ca;
                const double ratio = m_p.nibRatio > 0 ? m_p.nibRatio : 0.25;
                d = std::sqrt(u * u + (v / ratio) * (v / ratio));
            } else d = m_p.square ? qMax(qAbs(dx), qAbs(dy)) : std::sqrt(dx * dx + dy * dy);
            const double aa = qBound(0.0, r - d + 0.5, 1.0);
            if (aa <= 0) continue;
            double w = edge > 0 ? aa * retouch::edgeWeight(d, r + 0.5, edge) : aa;
            switch (cat) {
            case Category::Charcoal: w *= 0.25 + 0.75 * hashf(x, y, 3); break;
            case Category::Crayon: w *= hashf(x / 2, y / 2, 7) > 0.35f ? 1.0 : 0.15; break;
            case Category::FeltTip: w *= 0.85; break;
            default: break;
            }
            w *= opK * inkK;
            float &v = m[x];
            const double flowAmt = customFlow > 0 ? customFlow : 1.0;
            if (cat == Category::AirBrush) v = float(v + (1 - v) * w * 0.12 * flowK * flowAmt * (customBuild ? 2.0 : 1.0));
            else if (cat == Category::Watercolor) v = float(qMin(0.85, v + (1 - v) * w * 0.2 * flowK * flowAmt));
            else if (flow) v = float(v + (1 - v) * w * flowK * 0.5);   // flow: dabs build up with the pressure
            else if (customFlow > 0) v = float(v + (1 - v) * w * customFlow * 0.5);
            else if (customBuild) v = float(v + (1 - v) * w);
            else if (w > v) v = float(w);
        }
    }
}

void Stroke::pencilLine(QPointF a, QPointF b, QRect *dirty, int n) {
    n = qMax(1, n);
    if (m_p.antialias) {
        // anti-aliased pencil: a painter line on a scratch mask, max-combined
        const QRectF area = QRectF(a, b).normalized().adjusted(-n - 2, -n - 2, n + 2, n + 2);
        const QRect R = area.toAlignedRect().intersected(QRect(0, 0, m_w, m_h));
        if (R.isEmpty()) return;
        QImage s(R.size(), QImage::Format_Alpha8);
        s.fill(0);
        QPainter p(&s);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-R.topLeft());
        p.setPen(QPen(Qt::black, n, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        if (a == b) { p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawRect(QRectF(a - QPointF(n / 2.0, n / 2.0), QSizeF(n, n))); }
        else p.drawLine(a, b);
        p.end();
        for (int y = 0; y < R.height(); ++y) {
            const uchar *sl = s.constScanLine(y);
            float *m = m_mask.data() + (R.top() + y) * m_w + R.left();
            for (int x = 0; x < R.width(); ++x) m[x] = qMax(m[x], sl[x] / 255.f);
        }
        *dirty |= R;
        return;
    }
    // aliased: Bresenham, an n x n square of pixels at each step
    QPoint p0(qFloor(a.x()), qFloor(a.y())), p1(qFloor(b.x()), qFloor(b.y()));
    int dx = qAbs(p1.x() - p0.x()), dy = -qAbs(p1.y() - p0.y());
    const int sx = p0.x() < p1.x() ? 1 : -1, sy = p0.y() < p1.y() ? 1 : -1;
    int err = dx + dy;
    const int off = (n - 1) / 2;
    for (;;) {
        const QRect sq = QRect(p0.x() - off, p0.y() - off, n, n).intersected(QRect(0, 0, m_w, m_h));
        for (int y = sq.top(); y <= sq.bottom(); ++y)
            for (int x = sq.left(); x <= sq.right(); ++x) m_mask[y * m_w + x] = 1.f;
        if (!sq.isEmpty()) *dirty |= sq;
        if (p0 == p1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; p0.rx() += sx; }
        if (e2 <= dx) { err += dx; p0.ry() += sy; }
    }
}


namespace {
// The suffix of a moveTo/lineTo path starting at point startIdx, with sample
// distances shifted so they start at 0 on the suffix.
void engineTail(const QPainterPath &src, const QVector<double> &dist,
                const QVector<double> &press, const QVector<double> &speeds,
                const QVector<double> &times,
                int startIdx, QPainterPath *tail, stroke::PaintInput *in,
                int endIdx = -1)
{
    tail->clear();
    in->sampleDist.clear();
    in->samplePressure.clear();
    in->sampleSpeed.clear();
    in->sampleTime.clear();
    if (startIdx < 0) startIdx = 0;
    const int n = src.elementCount();
    if (endIdx < 0 || endIdx >= n) endIdx = n - 1;
    if (startIdx >= n || startIdx > endIdx) return;
    const double origin = (startIdx < dist.size()) ? dist[startIdx] : 0;
    in->pathOrigin = origin;
    // Keep absolute sample distances so walk()/pressureAt match a full-path
    // paint. (Shifting them to 0 restarted colour/RNG on every suffix.)
    // isEmpty() is still true after a lone moveTo, so a first-point test
    // must use elementCount(); otherwise every point becomes a moveTo and
    // the tail is discarded, forcing a full-path paint (cost grows with length).
    for (int i = startIdx; i <= endIdx; ++i) {
        const auto e = src.elementAt(i);
        const QPointF pt(e.x, e.y);
        if (tail->elementCount() == 0) tail->moveTo(pt);
        else tail->lineTo(pt);
        if (i < dist.size()) in->sampleDist.append(dist[i]);
        if (i < press.size()) in->samplePressure.append(press[i]);
        if (i < speeds.size()) in->sampleSpeed.append(speeds[i]);
        if (i < times.size()) in->sampleTime.append(times[i]);
    }
}
}

void Stroke::paintEngine(bool full) {
    if (m_enginePath.isEmpty()) {
        m_engineDirty = QRect();
        return;
    }
    stroke::PaintInput in;
    in.params = m_p.strokeCustom ? m_p.strokeParams : stroke::paramsFor(m_p.strokeKey);
    in.params.edge = m_p.edge;
    in.params.square = m_p.square;
    tablet::Sample pr;
    pr.pressure = m_enginePressure;
    in.width = (m_p.pressure & PressureSize) ? sizeAt(pr) : m_p.size;
    in.color = m_p.color;
    double opK = qBound(1, m_p.opacity, 100) / 100.0;
    if (m_p.pressure & PressureOpacity)
        opK *= tablet::withFloor(qBound(0.0, m_enginePressure, 1.0), qBound(0.0, m_p.minOpacity, 1.0));
    in.color.setAlphaF(float(in.color.alphaF() * opK));
    in.seed = m_engineSeed;
    in.pressure = m_enginePressure;
    in.speed = m_engineSpeed;
    in.sampleDist = m_engineDist;
    in.samplePressure = m_enginePressures;
    in.sampleSpeed = m_engineSpeeds;
    in.sampleTime = m_engineTimes;
    if (in.params.look == stroke::Look::ImageStamp)
        in.stampImage = stroke::loadStampImage(in.params.stampSource);
    if (in.params.look == stroke::Look::Cellular
        && (in.params.cellSampleUnder || in.params.cellFollowEdges))
        in.underImage = m_base;
    if (in.params.look == stroke::Look::Cellular) {
        if (m_cellCs <= 0) m_cellCs = qBound(1, in.params.cellSize, 32);
        in.cellSizeLock = m_cellCs;
    }
    const double ext = stroke::paintedRadius(in.params, in.width) + 2;
    auto boundsOf = [&]() {
        return stroke::paintedBounds(m_enginePath, in.params, in.width)
                   .adjusted(-2, -2, 2, 2).toAlignedRect()
                   .intersected(QRect(0, 0, m_w, m_h));
    };
    QRect pathDirty;
    if (full) pathDirty = boundsOf();
    if (m_enginePaint.width() < m_w || m_enginePaint.height() < m_h) {
        const int cw = qMax(m_w, m_capW), ch = qMax(m_h, m_capH);
        QImage np(cw, ch, QImage::Format_ARGB32_Premultiplied);
        np.fill(0);
        if (!m_enginePaint.isNull()) {
            QPainter gp(&np);
            gp.setCompositionMode(QPainter::CompositionMode_Source);
            gp.drawImage(0, 0, m_enginePaint);
        }
        m_enginePaint = np;
        m_capW = qMax(m_capW, cw);
        m_capH = qMax(m_capH, ch);
        full = true;
    }
    const bool pathGlobal = stroke::pathGlobalLook(in.params);
    // Looks whose earlier pixels change as the path grows must always be a full redraw.
    if (pathGlobal) full = true;
    if (full && pathDirty.isEmpty()) pathDirty = boundsOf();
    QRect dirty = pathDirty;
    if (!full && m_engineHasLast) {
        // QRectF(p, p) is null; QRectF::united ignores a null rect, so a
        // last-to-current tail built that way collapsed to the current point
        // only and left a gap in the middle of the new segment.
        const QPointF cur = m_enginePath.currentPosition();
        const qreal x0 = qMin(m_engineLast.x(), cur.x());
        const qreal y0 = qMin(m_engineLast.y(), cur.y());
        QRectF tailF(x0, y0, qAbs(cur.x() - m_engineLast.x()), qAbs(cur.y() - m_engineLast.y()));
        tailF.adjust(-ext, -ext, ext, ext);
        if (in.params.look == stroke::Look::Smoke)
            tailF.adjust(0, -0.16 * m_enginePathLen, 0, 0);
        const QRect tail = tailF.toAlignedRect().intersected(QRect(0, 0, m_w, m_h));
        if (!tail.isEmpty()) dirty = tail;
    }
    if (!full && in.params.look == stroke::Look::Cellular)
        in.gridLimit = dirty;
    // Incremental: clear a padded dirty rect and repaint every path run that
    // can touch it. Painting only the trailing suffix left rectangular seams
    // when an earlier coil or a wide effect reached into the cleared rect;
    // falling back to a full-path paint made cost grow with length.
    constexpr int kMinClipPad = 8;
    const int clipPad = qMax(kMinClipPad, int(std::ceil(ext)) / 4 + 2);
    QRect paintR = dirty;
    if (!full)
        paintR = dirty.adjusted(-clipPad, -clipPad, clipPad, clipPad).intersected(QRect(0, 0, m_w, m_h));
    QPainterPath paintPath = m_enginePath;
    struct PathRun { int a = 0, b = 0; };
    QVector<PathRun> runs;
    const bool additiveSpray = in.params.look == stroke::Look::SprayCan && in.params.sprayLook >= 1;
    // Looks whose stroke of a suffix is not a subset of the full-path stroke
    // (outline/dash of the whole path, etc.) must clip-paint the full path.
    // Offset bristles of a suffix do not match a full-path offset (end normals,
    // shorter polylines). Clip-paint the full path; cost stays modest.
    const bool clipWhole = in.params.look == stroke::Look::Bristle
                        || in.params.look == stroke::Look::Stitch;
    bool sprayRebuild = false;
    if (!full && !paintR.isEmpty() && m_enginePath.elementCount() > 2 && !clipWhole) {
        const QRect infl = paintR.adjusted(-int(std::ceil(ext)), -int(std::ceil(ext)),
                                           int(std::ceil(ext)), int(std::ceil(ext)));
        const int n = m_enginePath.elementCount();
        auto extentHits = [&](int i) {
            const auto e = m_enginePath.elementAt(i);
            const QPointF pt(e.x, e.y);
            return infl.intersects(QRect(int(std::floor(pt.x() - ext)), int(std::floor(pt.y() - ext)),
                                          int(std::ceil(2 * ext)) + 2, int(std::ceil(2 * ext)) + 2));
        };
        auto pathDist = [&](int i, int j) {
            if (i > j) std::swap(i, j);
            if (j < m_engineDist.size() && i < m_engineDist.size())
                return std::abs(m_engineDist[j] - m_engineDist[i]);
            return 0.0;
        };
        // Vertices are often closer together than the stroke radius. Pad each
        // run by at least ext so a RoundCap or dab at the run end sits
        // outside the clip, matching a continuous full-path stroke.
        // Path-distance pad is not enough on a tight curve: a short arc can
        // stay Euclidean-close to the clip, so also expand until a disc of
        // radius ext around the endpoint does not meet paintR.
        const double padDist = ext + clipPad + 1;
        auto outsideClip = [&](int i) {
            const auto e = m_enginePath.elementAt(i);
            const QRect extent(int(std::floor(e.x - ext)),
                               int(std::floor(e.y - ext)),
                               int(std::ceil(2 * ext)) + 2,
                               int(std::ceil(2 * ext)) + 2);
            return !extent.intersects(paintR);
        };
        auto expand = [&](int &a, int &b, int firstHit, int lastHit) {
            while (a > 0 && (pathDist(a, firstHit) < padDist || !outsideClip(a))) --a;
            while (b < n - 1 && (pathDist(lastHit, b) < padDist || !outsideClip(b))) ++b;
        };
        if (additiveSpray) {
            // Additive: do not clear or redo earlier dabs. Paint only the new
            // suffix, without backward pad — expanding would stamp the overlap
            // twice and double-build the spray.
            // Resume at the last already-stamped vertex (i0 will skip the join).
            // Starting at the dirty-rect hit list re-sprayed earlier dabs and
            // made the incremental buffer much denser than a full paint.
            const int startIdx = (m_enginePaintedN > 0) ? qMin(n - 1, m_enginePaintedN - 1) : 0;
            // When the new tail overlaps an earlier coil, a suffix stamp on
            // top of already-composited dabs is not the same as one full
            // paint of the whole path. Clear the dirty rect and clip-paint
            // the stroke, as other looks do on self-overlap.
            // Do not use the dirty-rect hit list: Spray Can's radius is large
            // enough that a long straight tail still "hits" earlier vertices
            // of the same simple stroke, which would force a full rebuild
            // and make per-event cost grow with length.
            QRect suffixInfl;
            bool haveBox = false;
            for (int i = startIdx; i < n; ++i) {
                const auto e = m_enginePath.elementAt(i);
                const QRect box(int(std::floor(e.x - ext)), int(std::floor(e.y - ext)),
                                int(std::ceil(2 * ext)) + 2, int(std::ceil(2 * ext)) + 2);
                suffixInfl = haveBox ? suffixInfl.united(box) : box;
                haveBox = true;
            }
            // Walk back along the path while it still sits in the suffix
            // neighbourhood (the simple precursor). A path-distance pad is
            // shorter than the Euclidean spray radius, so earlier vertices
            // of a long straight stroke would otherwise look like a coil.
            int pred = startIdx;
            while (pred > 0) {
                const auto e = m_enginePath.elementAt(pred - 1);
                const QRect box(int(std::floor(e.x - ext)), int(std::floor(e.y - ext)),
                                int(std::ceil(2 * ext)) + 2, int(std::ceil(2 * ext)) + 2);
                if (!suffixInfl.intersects(box)) break;
                --pred;
            }
            for (int i = 0; i < pred; ++i) {
                const auto e = m_enginePath.elementAt(i);
                const QRect box(int(std::floor(e.x - ext)), int(std::floor(e.y - ext)),
                                int(std::ceil(2 * ext)) + 2, int(std::ceil(2 * ext)) + 2);
                if (suffixInfl.intersects(box)) { sprayRebuild = true; break; }
            }
            if (sprayRebuild)
                runs.clear();
            else
                runs.append({startIdx, n - 1});
        } else {
            int i = 0;
            while (i < n) {
                while (i < n && !extentHits(i)) ++i;
                if (i >= n) break;
                int firstHit = i, lastHit = i;
                while (lastHit + 1 < n && extentHits(lastHit + 1)) ++lastHit;
                int a = firstHit, b = lastHit;
                expand(a, b, firstHit, lastHit);
                if (!runs.isEmpty() && a <= runs.last().b)
                    runs.last().b = qMax(runs.last().b, b);
                else
                    runs.append({a, b});
                i = lastHit + 1;
            }
        }
        const bool whole = (runs.size() == 1 && runs[0].a == 0 && runs[0].b >= n - 1);
        if (whole) {
            runs.clear(); // paint the existing full path / full samples
        }
    }
    if (full) {
        if (pathDirty.isEmpty()) pathDirty = boundsOf();
        {
            QPainter clr(&m_enginePaint);
            clr.setCompositionMode(QPainter::CompositionMode_Source);
            const QRect clearR = m_engineDirty.united(pathDirty).intersected(QRect(0, 0, m_w, m_h));
            if (!clearR.isEmpty()) clr.fillRect(clearR, Qt::transparent);
        }
        dirty = pathDirty;
        paintR = pathDirty;
        paintPath = m_enginePath;
        in.sampleDist = m_engineDist;
        in.samplePressure = m_enginePressures;
        in.sampleSpeed = m_engineSpeeds;
        in.sampleTime = m_engineTimes;
        in.pathOrigin = 0;
        runs.clear();
    } else if (!paintR.isEmpty() && (!additiveSpray || sprayRebuild)) {
        QPainter clr(&m_enginePaint);
        clr.setCompositionMode(QPainter::CompositionMode_Source);
        clr.fillRect(paintR, Qt::transparent);
    }
    if (!paintR.isEmpty()) {
        QPainter p(&m_enginePaint);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (!full) p.setClipRect(paintR);
        if (!full && !runs.isEmpty() && runs.size() == 1) {
            for (const PathRun &r : runs) {
                QPainterPath tail;
                stroke::PaintInput inRun = in;
                engineTail(m_enginePath, m_engineDist, m_enginePressures, m_engineSpeeds, m_engineTimes,
                           r.a, &tail, &inRun, r.b);
                if (tail.elementCount() == 0) continue;
                stroke::paint(p, tail, inRun);
            }
        } else {
            // Full path (possibly clipped): either a genuine full paint, a
            // look that cannot split, or a self-overlapping dirty rect where
            // separate runs would not match one continuous strokePath.
            in.pathOrigin = 0;
            stroke::paint(p, paintPath, in);
        }
    }
    // Incremental clears and paints paintR (dirty + clip pad). Returning only
    // the unpadded tail left a halo that finish() later composited, so live
    // and mouse-up images diverged.
    const QRect newDirty = full ? pathDirty : (paintR.isEmpty() ? dirty : paintR);
    m_engineLastDirty = newDirty;
    m_engineDirty = full ? pathDirty : (m_engineDirty.isNull() ? newDirty : m_engineDirty.united(newDirty));
    m_engineLastWasFull = full;
    m_enginePending = false;
    m_enginePaintedN = m_enginePath.elementCount();
    if (full)
        m_ckpts.clear();
}

void Stroke::takeEngineCheckpoint()
{
    if (m_enginePaint.isNull() || m_engineLastDirty.isEmpty()) return;
    const int idx = m_enginePath.elementCount();
    if (!m_ckpts.isEmpty() && m_ckpts.last().index == idx) return;
    EngineCkpt ck;
    ck.index = idx;
    // Snapshot only a local window around the latest paint. Copying the
    // accumulated dirty union grew with stroke length (O(n) per checkpoint).
    QRect window = m_engineLastDirty;
    if (m_engineHasLast || !m_enginePath.isEmpty()) {
        const QPointF cur = m_enginePath.isEmpty() ? m_engineLast : m_enginePath.currentPosition();
        constexpr int rad = 256;
        window = window.united(QRect(int(std::floor(cur.x())) - rad,
                                     int(std::floor(cur.y())) - rad,
                                     rad * 2, rad * 2));
    }
    ck.rect = window.intersected(m_enginePaint.rect());
    if (ck.rect.isEmpty()) return;
    ck.tile = m_enginePaint.copy(ck.rect);
    m_ckpts.append(std::move(ck));
    while (m_ckpts.size() > 12)
        m_ckpts.removeFirst();
}

QRect Stroke::dwell()
{
    if (!m_useEngine || !active() || m_enginePath.isEmpty() || m_engineFinalized)
        return QRect();
    const QPointF pt = m_enginePath.currentPosition();
    tablet::Sample s;
    s.pressure = m_enginePressure;
    return segment(pt, pt, s, s);
}

QRect Stroke::repaintEngineFull()
{
    if (!m_useEngine || m_enginePath.isEmpty()) return QRect();
    paintEngine(true);
    return m_engineDirty;
}

QRect Stroke::flushEngine()
{
    if (!m_useEngine) return QRect();
    if (!m_enginePending && !m_enginePaint.isNull())
        return m_engineLastDirty;
    if (m_enginePath.isEmpty()) {
        m_enginePending = false;
        return QRect();
    }
    paintEngine(false);
    m_engineLast = m_enginePath.currentPosition();
    m_engineHasLast = true;
    return m_engineLastDirty;
}

PathSnapshot Stroke::capturePathSnapshot(int fromIndex, quint64 gen) const
{
    PathSnapshot s;
    s.params = m_p;
    s.seed = m_engineSeed;
    s.cellCs = m_cellCs;
    s.size = QSize(m_w, m_h);
    s.fromIndex = qMax(0, fromIndex);
    s.elementCount = m_enginePath.elementCount();
    s.gen = gen;
    // Deep-copy the path so a later UI-thread lineTo cannot race implicit sharing.
    const int n = m_enginePath.elementCount();
    for (int i = 0; i < n; ++i) {
        const auto e = m_enginePath.elementAt(i);
        const QPointF pt(e.x, e.y);
        if (i == 0) s.path.moveTo(pt);
        else s.path.lineTo(pt);
    }
    s.dist = m_engineDist;
    s.dist.detach();
    s.pressure = m_enginePressures;
    s.pressure.detach();
    s.speed = m_engineSpeeds;
    s.speed.detach();
    s.time = m_engineTimes;
    s.time.detach();
    const stroke::Params sp = m_p.strokeCustom ? m_p.strokeParams
        : (m_p.strokeKey.isEmpty() ? stroke::Params{} : stroke::paramsFor(m_p.strokeKey));
    if (sp.look == stroke::Look::Cellular
        && (sp.cellSampleUnder || sp.cellFollowEdges)
        && !m_base.isNull())
        s.under = m_base.copy(QRect(0, 0, m_w, m_h));
    return s;
}

QRect Stroke::adoptEngineLayer(const QImage &layer)
{
    if (!m_useEngine || layer.isNull()) return QRect();
    if (layer.size() != QSize(m_w, m_h)
        && !(layer.width() >= m_w && layer.height() >= m_h))
        return QRect();
    if (m_enginePaint.width() < m_w || m_enginePaint.height() < m_h
        || m_enginePaint.format() != QImage::Format_ARGB32_Premultiplied) {
        m_enginePaint = QImage(qMax(m_w, m_capW), qMax(m_h, m_capH),
                               QImage::Format_ARGB32_Premultiplied);
        m_enginePaint.fill(0);
        m_capW = qMax(m_capW, m_enginePaint.width());
        m_capH = qMax(m_capH, m_enginePaint.height());
    }
    QPainter p(&m_enginePaint);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(0, 0, layer);
    p.end();
    m_engineDirty = QRect(0, 0, m_w, m_h);
    m_engineLastDirty = m_engineDirty;
    m_enginePending = false;
    m_engineLastWasFull = true;
    m_enginePaintedN = m_enginePath.elementCount();
    if (!m_enginePath.isEmpty()) {
        m_engineLast = m_enginePath.currentPosition();
        m_engineHasLast = true;
    }
    return m_engineDirty;
}

void Stroke::copyStroke(QImage &layer, const QRect &rIn, bool replace) const
{
    if (!active() || layer.isNull()) return;
    const QRect r = rIn.intersected(QRect(0, 0, m_w, m_h)).intersected(layer.rect());
    if (r.isEmpty()) return;
    if (!m_enginePaint.isNull()) {
        QPainter p(&layer);
        p.setCompositionMode(replace ? QPainter::CompositionMode_Source
                                     : QPainter::CompositionMode_SourceOver);
        p.drawImage(r.topLeft(), m_enginePaint, r);
        return;
    }
    const double k = qBound(1, m_p.opacity, 100) / 100.0;
    const QColor c = m_p.erase ? QColor(Qt::black) : m_p.color;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        QRgb *l = reinterpret_cast<QRgb *>(layer.scanLine(y)) + r.left();
        const float *m = m_mask.constData() + y * m_w + r.left();
        const uchar *cl = m_clip.isNull() ? nullptr : m_clip.constScanLine(y) + r.left();
        for (int x = 0; x < r.width(); ++x) {
            double a = m[x] * k * c.alphaF();
            if (a > 0) a *= texFactor(m_tex, m_p.textureAmount, r.left() + x, y);
            if (cl) a *= cl[x] / 255.0;
            const int ai = qBound(0, int(std::lround(a * 255)), 255);
            const QRgb src = qPremultiply(qRgba(c.red(), c.green(), c.blue(), ai));
            if (replace || ai == 0) {
                if (replace) l[x] = src;
            } else {
                // SourceOver into an already-premultiplied dest
                const QRgb dst = l[x];
                const int da = qAlpha(dst);
                const int ia = 255 - ai;
                const int ra = ai + (da * ia + 127) / 255;
                auto ch = [&](int s, int d) {
                    return ra ? qBound(0, (s * 255 + d * ia + 127) / 255, 255) : 0;
                };
                l[x] = qRgba(ch(qRed(src), qRed(dst)),
                             ch(qGreen(src), qGreen(dst)),
                             ch(qBlue(src), qBlue(dst)),
                             ra);
            }
        }
    }
}

QImage renderPathLayer(const PathSnapshot &snap)
{
    QImage out(qMax(1, snap.size.width()), qMax(1, snap.size.height()),
               QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    if (snap.path.isEmpty() || snap.size.isEmpty()) return out;

    stroke::PaintInput in;
    in.params = snap.params.strokeCustom ? snap.params.strokeParams
                                         : stroke::paramsFor(snap.params.strokeKey);
    in.params.edge = snap.params.edge;
    in.params.square = snap.params.square;
    tablet::Sample pr;
    pr.pressure = snap.pressure.isEmpty() ? 1.0 : snap.pressure.last();
    auto sizeAt = [&](const tablet::Sample &s) {
        if (!(snap.params.pressure & PressureSize)) return snap.params.size;
        return qMax(snap.params.pencil ? 1.0 : 0.5,
                    snap.params.size * tablet::withFloor(qBound(0.0, s.pressure, 1.0),
                                                         qBound(0.0, snap.params.minSize, 1.0)));
    };
    in.width = (snap.params.pressure & PressureSize) ? sizeAt(pr) : snap.params.size;
    in.color = snap.params.color;
    double opK = qBound(1, snap.params.opacity, 100) / 100.0;
    if (snap.params.pressure & PressureOpacity) {
        const double p = snap.pressure.isEmpty() ? 1.0 : snap.pressure.last();
        opK *= tablet::withFloor(qBound(0.0, p, 1.0), qBound(0.0, snap.params.minOpacity, 1.0));
    }
    in.color.setAlphaF(float(in.color.alphaF() * opK));
    in.seed = snap.seed;
    in.pressure = snap.pressure.isEmpty() ? 1.0 : snap.pressure.last();
    in.speed = snap.speed.isEmpty() ? 1.0 : snap.speed.last();
    in.sampleDist = snap.dist;
    in.samplePressure = snap.pressure;
    in.sampleSpeed = snap.speed;
    in.sampleTime = snap.time;
    if (in.params.look == stroke::Look::ImageStamp)
        in.stampImage = stroke::loadStampImage(in.params.stampSource);
    if (in.params.look == stroke::Look::Cellular
        && (in.params.cellSampleUnder || in.params.cellFollowEdges))
        in.underImage = snap.under;
    if (in.params.look == stroke::Look::Cellular)
        in.cellSizeLock = snap.cellCs > 0 ? snap.cellCs : qBound(1, in.params.cellSize, 32);

    QPainterPath paintPath = snap.path;
    if (snap.fromIndex > 0 && snap.fromIndex < snap.path.elementCount()) {
        QPainterPath tail;
        in.pathOrigin = (snap.fromIndex < snap.dist.size()) ? snap.dist[snap.fromIndex] : 0;
        in.sampleDist.clear();
        in.samplePressure.clear();
        in.sampleSpeed.clear();
        in.sampleTime.clear();
        const int n = snap.path.elementCount();
        for (int i = snap.fromIndex; i < n; ++i) {
            const auto e = snap.path.elementAt(i);
            const QPointF pt(e.x, e.y);
            if (tail.elementCount() == 0) tail.moveTo(pt);
            else tail.lineTo(pt);
            if (i < snap.dist.size()) in.sampleDist.append(snap.dist[i]);
            if (i < snap.pressure.size()) in.samplePressure.append(snap.pressure[i]);
            if (i < snap.speed.size()) in.sampleSpeed.append(snap.speed[i]);
            if (i < snap.time.size()) in.sampleTime.append(snap.time[i]);
        }
        paintPath = tail;
    } else {
        in.pathOrigin = 0;
    }
    if (paintPath.elementCount() == 0) return out;

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    stroke::paint(p, paintPath, in);
    p.end();
    return out;
}

QRect Stroke::segment(QPointF a, QPointF b) { return segment(a, b, tablet::Sample(), tablet::Sample()); }

QRect Stroke::segment(QPointF a, QPointF b, const tablet::Sample &sa, const tablet::Sample &sb) {
    QRect dirty;
    if (!active()) return dirty;
    if (m_useEngine) {
        m_enginePressure = qBound(0.0, sb.pressure, 1.0);
        if (m_first) {
            m_enginePath.moveTo(a);
            m_engineDist.append(0);
            m_enginePressures.append(qBound(0.0, sa.pressure > 0 ? sa.pressure : sb.pressure, 1.0));
            m_engineSpeeds.append(1);
            m_engineTimes.append(0);
            if (QLineF(a, b).length() > 0.25) m_enginePath.lineTo(b);
            else {
                // A click (zero-length segment) must still leave a mark; a moveTo-only
                // QPainterPath is empty and stroke::paint would be a no-op.
                m_enginePath.lineTo(b + QPointF(0.05, 0));
            }
            m_first = false;
        } else {
            const double len = QLineF(a, b).length();
            m_engineSpeed = qBound(0.05, len / qMax(1.0, m_p.size * 0.8), 2.0);
            m_enginePath.lineTo(b);
        }
        const double seglen = QLineF(a, b).length();
        m_enginePathLen += seglen;
        m_engineDist.append(m_enginePathLen);
        m_enginePressures.append(m_enginePressure);
        m_engineSpeeds.append(m_engineSpeed);
        m_engineTimes.append(double(m_engineClock.isValid() ? m_engineClock.elapsed() : 0));
        // A dwell (zero-length) must still add a path element so times stay aligned.
        if (m_enginePath.elementCount() < m_engineTimes.size())
            m_enginePath.lineTo(b + QPointF(0.02, 0));
        m_enginePathBounds = m_enginePathBounds.united(QRectF(a, b).normalized());
        m_engineFinalized = false;
        // Incremental padded-tail paint (see paintEngine). Canvas can defer
        // this to once per frame so a burst of tablet events does not queue
        // a full paint each. m_engineLast is the last *painted* point, so a
        // flush still covers the whole unpainted suffix. finish() flushes
        // pending points without a second full-path paint, so live matches
        // the commit.
        if (m_engineDeferred) {
            m_enginePending = true;
            return QRect();
        }
        paintEngine(false);
        m_engineLast = b;
        m_engineHasLast = true;
        return m_engineLastDirty;
    }
    if (m_p.pencil) {
        // the Pencil follows the pressure with its size only (whole pixels); split where the size changes
        if (m_first || !(m_p.pressure & PressureSize)) {
            pencilLine(m_first ? b : a, b, &dirty, qRound(sizeAt(sb)));
            m_first = false;
            return dirty;
        }
        const double len = QLineF(a, b).length();
        const int steps = qMax(1, int(std::ceil(len)));
        QPointF from = a;
        int nFrom = qRound(sizeAt(sa));
        for (int i = 1; i <= steps; ++i) {
            const double t = double(i) / steps;
            const int n = qRound(sizeAt(tablet::lerp(sa, sb, t)));
            if (n != nFrom || i == steps) {
                const QPointF to = a + (b - a) * t;
                pencilLine(from, to, &dirty, i == steps ? n : nFrom);
                from = to;
                nFrom = n;
            }
        }
        return dirty;
    }
    if (m_first) {
        dab(b, &dirty, sb);
        m_first = false;
        m_left = dabStep(sb);
        return dirty;
    }
    const double len = QLineF(a, b).length();
    double t = m_left;
    while (t <= len) {
        const double f = t / qMax(1e-9, len);
        const tablet::Sample s = tablet::lerp(sa, sb, f);
        dab(a + (b - a) * f, &dirty, s);
        t += dabStep(s);   // smaller dabs sit closer together
    }
    m_left = t - len;
    return dirty;
}

QRect Stroke::strokePath(const QPainterPath &path)
{
    QRect dirty;
    if (!active() || path.isEmpty()) return dirty;
    if (m_useEngine) {
        m_enginePath = path;
        m_enginePathBounds = path.controlPointRect();
        m_enginePathLen = path.length();
        m_engineDist.clear();
        m_enginePressures.clear();
        m_engineSpeeds.clear();
        if (m_previewSense) {
            stroke::PaintInput tmp;
            stroke::fillPreviewSenseRamp(tmp, path);
            m_engineDist = tmp.sampleDist;
            m_enginePressures = tmp.samplePressure;
            m_engineSpeeds = tmp.sampleSpeed;
        } else {
            m_engineDist.append(0);
            m_enginePressures.append(m_enginePressure);
            m_engineSpeeds.append(m_engineSpeed);
            if (m_enginePathLen > 0) {
                m_engineDist.append(m_enginePathLen);
                m_enginePressures.append(m_enginePressure);
                m_engineSpeeds.append(m_engineSpeed);
            }
        }
        m_first = false;
        m_engineFinalized = false;
        paintEngine(true);
        m_engineFinalized = true;
        return m_engineDirty;
    }
    const double len = path.length();
    const QPointF start = path.pointAtPercent(0);
    if (len <= 0.25) {
        dirty |= segment(start, start);
        return dirty;
    }
    // Step small enough for the dab engine and the Pencil to follow the curve.
    const double step = qMax(0.6, qMin(2.0, m_p.size * 0.15));
    QPointF prev = start;
    dirty |= segment(prev, prev);
    for (double d = step; d < len; d += step) {
        const QPointF pt = path.pointAtPercent(qBound(0.0, d / len, 1.0));
        dirty |= segment(prev, pt);
        prev = pt;
    }
    dirty |= segment(prev, path.pointAtPercent(1));
    return dirty;
}

QRect Stroke::finish()
{
    if (m_useEngine && !m_engineFinalized) {
        // Flush any deferred points. Do not start a second full-path paint:
        // that would stall on a heavy tip and could pop (live already shows
        // the stroke; it must match what finish commits).
        if (m_enginePending)
            paintEngine(false);
        m_engineFinalized = true;
    }
    return m_engineDirty;
}

void Stroke::composite(QImage &target, const QRect &rIn) {
    const QRect r = rIn.intersected(QRect(0, 0, m_w, m_h)).intersected(target.rect());
    if (r.isEmpty() || !active()) return;
    if (!m_enginePaint.isNull()) {
        QPainter p(&target);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(r.topLeft(), m_base, r);
        const bool plain = m_clip.isNull() && (m_tex.isNull() || m_p.textureAmount <= 0)
            && !m_p.erase && !m_p.preserveTransparency && m_p.blend == BlendMode::Normal;
        if (plain) {
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.drawImage(r.topLeft(), m_enginePaint, r);
            return;
        }
        QImage layer = m_enginePaint.copy(r);
        if (!m_clip.isNull() || (!m_tex.isNull() && m_p.textureAmount > 0)) {
            for (int y = 0; y < r.height(); ++y) {
                QRgb *l = reinterpret_cast<QRgb *>(layer.scanLine(y));
                const int ty = r.top() + y;
                const uchar *cl = m_clip.isNull() ? nullptr : m_clip.constScanLine(ty) + r.left();
                for (int x = 0; x < r.width(); ++x) {
                    double a = qAlpha(l[x]) / 255.0;
                    if (a <= 0) continue;
                    a *= texFactor(m_tex, m_p.textureAmount, r.left() + x, ty);
                    if (cl) a *= cl[x] / 255.0;
                    const int ai = qBound(0, int(std::lround(a * 255)), 255);
                    const QRgb c = qUnpremultiply(l[x]);
                    l[x] = qPremultiply(qRgba(qRed(c), qGreen(c), qBlue(c), ai));
                }
            }
        }
        if (m_p.erase) {
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(r.topLeft(), layer);
            return;
        }
        if (m_p.preserveTransparency && m_p.blend == BlendMode::Normal) {
            p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
            p.drawImage(r.topLeft(), layer);
            return;
        }
        drawBlended(p, layer, QRect(r.topLeft(), layer.size()), m_p.blend, 1);
        if (m_p.preserveTransparency) {
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(r.topLeft(), m_base, r);
        }
        return;
    }
    const double k = qBound(1, m_p.opacity, 100) / 100.0;
    QImage layer(r.size(), QImage::Format_ARGB32_Premultiplied);
    const QColor c = m_p.erase ? QColor(Qt::black) : m_p.color;
    for (int y = 0; y < r.height(); ++y) {
        QRgb *l = reinterpret_cast<QRgb *>(layer.scanLine(y));
        const int ty = r.top() + y;
        const float *m = m_mask.constData() + ty * m_w + r.left();
        const uchar *cl = m_clip.isNull() ? nullptr : m_clip.constScanLine(ty) + r.left();
        for (int x = 0; x < r.width(); ++x) {
            double a = m[x] * k * c.alphaF();
            if (a > 0) a *= texFactor(m_tex, m_p.textureAmount, r.left() + x, ty);
            if (cl) a *= cl[x] / 255.0;
            const int ai = qBound(0, int(std::lround(a * 255)), 255);
            l[x] = qPremultiply(qRgba(c.red(), c.green(), c.blue(), ai));
        }
    }
    QPainter p(&target);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(r.topLeft(), m_base, r);
    if (m_p.erase) {
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.drawImage(r.topLeft(), layer);
        return;
    }
    if (m_p.preserveTransparency && m_p.blend == BlendMode::Normal) {
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.drawImage(r.topLeft(), layer);
        return;
    }
    drawBlended(p, layer, QRect(r.topLeft(), layer.size()), m_p.blend, 1);
    if (m_p.preserveTransparency) {
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(r.topLeft(), m_base, r);
    }
}

void fillMask(QImage &img, const QImage &mask, const QColor &c, int opacity, BlendMode blend, const QString &texture,
              int textureAmount, bool preserveTransparency) {
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    Stroke s;
    Params p;
    p.color = c;
    p.opacity = opacity;
    p.blend = blend;
    p.texture = texture;
    p.textureAmount = textureAmount;
    p.preserveTransparency = preserveTransparency;
    s.begin(img, p);
    QRect box;
    const QImage m = mask.convertToFormat(QImage::Format_Alpha8);
    for (int y = 0; y < qMin(m.height(), img.height()); ++y) {
        const uchar *sl = m.constScanLine(y);
        for (int x = 0; x < qMin(m.width(), img.width()); ++x)
            if (sl[x]) { s.m_mask[y * s.m_w + x] = sl[x] / 255.f; box |= QRect(x, y, 1, 1); }
    }
    s.composite(img, box);
}

static stroke::Params previewStrokeParams(const Params &bp)
{
    if (bp.strokeCustom) return bp.strokeParams;
    if (!bp.strokeKey.isEmpty()) return stroke::paramsFor(bp.strokeKey);
    return stroke::Params{};
}

QImage renderPreviewRaw(const Params &bp0, const QSize &dest, quint32 seed)
{
    Params bp = bp0;
    bp.size = qMax(bp.pencil ? 1.0 : 0.5, bp.size);
    const stroke::Params sp = previewStrokeParams(bp);
    const stroke::PreviewLayout L = stroke::previewLayout(sp, bp.size, dest);
    QImage img(L.work, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    if (L.path.isEmpty()) return img;
    Params paintBp = bp;
    paintBp.size = bp.size * L.scale;
    if (paintBp.strokeCustom) paintBp.strokeParams.size = paintBp.size;
    Stroke st;
    st.begin(img, paintBp);
    if (seed)
        st.setEngineSeed(seed);
    st.setPreviewSense(true);
    st.strokePath(L.path);
    st.finish();
    st.composite(img, img.rect());
    st.end();
    return img;
}

QImage renderPreview(const Params &bp, const QSize &dest, const QColor &background, quint32 seed)
{
    return stroke::fitPreview(renderPreviewRaw(bp, dest, seed), dest, background);
}
}
