#include <QDialog>
#include "DocumentView.h"
#include <atomic>
#include <functional>
#include "Shortcuts.h"
#include <QSettings>
#include <QFocusEvent>
#include "ViewGeometry.h"
#include <cmath>
#include "Rulers.h"
#include "AppState.h"
#include "io/Codecs.h"
#include <QFileInfo>
#include <QGridLayout>
#include "Canvas.h"
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QGridLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include "io/DocIO.h"
#include "io/Quantize.h"
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace {
QVector<QRgb> distinctColors(const QImage &img, int limit) {
    QSet<QRgb> seen;
    QVector<QRgb> out;
    const QImage im = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        for (int x = 0; x < im.width(); ++x) {
            const QRgb c = qAlpha(s[x]) == 0 ? 0u : s[x];
            if (seen.contains(c)) continue;
            if (out.size() >= limit) return {};
            seen.insert(c);
            out << c;
        }
    }
    return out;
}
}

EncodeResult encodePreview(const Document &doc, const ExportOptions &o, int frame, QByteArray *reuseBuf) {
    EncodeResult r;
    QElapsedTimer t;
    t.start();
    if (!ImageIO::available(o.format)) { r.error = QStringLiteral("%1 support is not built in").arg(ImageIO::formatName(o.format)); return r; }
    const bool docFmt = o.format == Fmt::PSD || o.format == Fmt::ORA || o.format == Fmt::XCF || o.format == Fmt::SVG
                        || o.format == Fmt::SVGZ || o.format == Fmt::PDF || (o.format == Fmt::PNG && o.pngEditable);
    QByteArray localBuf;
    QByteArray &data = reuseBuf ? *reuseBuf : localBuf;
    data.clear();
    const bool anim = o.animated && ImageIO::canAnimate(o.format) && doc.s.frames.size() > 1;
    if (o.format == Fmt::CHeader) {
        // the header text is not an image: show what the target will display (quantised pixels)
        ImageSeq seq = doc.toImageSeq(anim);
        if (!anim && frame >= 0 && frame < doc.s.frames.size()) seq.frames[0] = {doc.render(frame, true), doc.s.frames[frame].delayMs};
        if (!anim && seq.frames.size() > 1) seq.frames.resize(1);
        for (auto &f : seq.frames) f.image = f.image.convertToFormat(QImage::Format_ARGB32);
        ExportOptions oo = o;
        if (!oo.area.isEmpty()) {   // Export Area
            bool ok = true;
            seq = ImageIO::cropSeq(seq, oo.area, &ok);
            if (!ok) { r.error = QStringLiteral("The export area lies outside the canvas"); return r; }
            oo.area = QRect();
        }
        if (oo.scalePct != 100) { seq = ImageIO::scaleSeq(seq, qBound(1, oo.scalePct, 1600), oo.scaleResample); oo.scalePct = 100; }
        if (oo.chName.trimmed().isEmpty()) oo.chName = doc.filePath.isEmpty() ? doc.title() : QFileInfo(doc.filePath).completeBaseName();
        if (oo.sourceName.isEmpty()) oo.sourceName = doc.filePath.isEmpty() ? doc.title() : QFileInfo(doc.filePath).fileName();
        if (oo.outputName.isEmpty()) oo.outputName = oo.chName + QStringLiteral(".h");
        codec::CHeaderData info;
        if (!codec::writeCHeader(data, seq, oo, &r.error, &info)) return r;
        r.bytes = data.size();
        r.pixelBytes = info.dataBytes + info.palette.size() * 4;
        r.frames = info.decoded.size();
        r.image = info.decoded.value(anim ? qBound(0, frame, int(info.decoded.size()) - 1) : 0);
        if (!info.palette.isEmpty()) r.colorTable = info.palette;
        r.detail = QStringLiteral("%1, %2 bytes/row").arg(cheader::identifier(oo.chName)).arg(info.stride);
        r.encoded = data;
        r.ms = t.elapsed();
        return r;
    }
    if (docFmt) {
        if (!docio::exportDocument(doc, frame, o, data, &r.error, &r.notes)) return r;
    } else {
        ImageSeq seq = doc.toImageSeq(anim);
        if (!anim && frame >= 0 && frame < doc.s.frames.size()) seq.frames[0] = {doc.render(frame, true), doc.s.frames[frame].delayMs};
        if (!ImageIO::writeData(data, seq, o, &r.error)) return r;
        if (o.tinyPng && (o.format == Fmt::PNG || o.format == Fmt::APNG)) {
            ExportOptions plain = o;
            plain.tinyPng = false;
            QByteArray ref;
            if (ImageIO::writeData(ref, seq, plain)) r.originalBytes = ref.size();
        }
    }
    r.bytes = data.size();
    ImageSeq back;
    if (o.format == Fmt::PDF || !ImageIO::readData(data, back, nullptr) || back.frames.isEmpty()) {
        // PDF (and anything Libreworks cannot read back): show the rendered frame
        r.error.clear();
        r.image = doc.render(qBound(0, frame, qMax(0, int(doc.s.frames.size()) - 1)), true);
        if (!o.area.isEmpty()) r.image = r.image.copy(o.area.normalized() & r.image.rect());
        if (o.scalePct != 100) r.image = ImageIO::scaleSeq(ImageSeq{QVector<AnimFrame>{AnimFrame{r.image, 100}}, r.image.width(), r.image.height()}, o.scalePct, o.scaleResample).frames[0].image;
        r.frames = o.format == Fmt::PDF ? doc.s.frames.size() : 1;
        r.encoded = data;
        r.ms = t.elapsed();
        return r;
    }
    r.frames = back.frames.size();
    int idx = anim ? qBound(0, frame, back.frames.size() - 1) : 0;
    r.image = back.frames[idx].image;
    if (o.format == Fmt::GIF || ((o.format == Fmt::PNG || o.format == Fmt::APNG) && (o.pngType == 2 || o.tinyPng)))
        r.colorTable = distinctColors(r.image, 256);
    r.encoded = data;
    r.ms = t.elapsed();
    return r;
}

QString describeSaving(const EncodeResult &r) {
    if (r.pixelBytes >= 0)
        return QStringLiteral("Data: %L1 bytes (header file %L2 bytes)%3").arg(r.pixelBytes).arg(r.bytes)
            .arg(r.detail.isEmpty() ? QString() : QStringLiteral("\n") + r.detail);
    if (r.originalBytes <= 0) return {};
    const double pct = 100.0 * (1.0 - double(r.bytes) / double(r.originalBytes));
    return QStringLiteral("Tiny: %1K from %2K (%3 % saved)").arg(r.bytes / 1024.0, 0, 'f', 1).arg(r.originalBytes / 1024.0, 0, 'f', 1).arg(pct, 0, 'f', 1);
}


OptimizeToSizeResult optimizeToSize(const Document &doc, const ExportOptions &options, int frame, qint64 targetBytes,
                                    std::atomic<bool> *cancel)
{
    ExportOptions o = options;
    OptimizeToSizeResult out;
    out.options = o;
    auto stopped = [&] { return cancel && cancel->load(); };
    QByteArray trialBuf;
    auto trial = [&](const ExportOptions &opt) -> EncodeResult {
        return encodePreview(doc, opt, frame, &trialBuf);
    };
    if (stopped()) { out.cancelled = true; return out; }
    out.encoded = trial(o);
    if (!out.encoded.error.isEmpty()) return out;

    const bool lossyQuality = (o.format == Fmt::JPEG || o.format == Fmt::HEIC
                               || ((o.format == Fmt::WEBP || o.format == Fmt::AVIF || o.format == Fmt::JXL) && !o.lossless));
    const bool indexed = o.format == Fmt::GIF
                         || ((o.format == Fmt::PNG || o.format == Fmt::APNG) && (o.pngType == 2 || o.tinyPng));
    const bool losslessPng = (o.format == Fmt::PNG || o.format == Fmt::APNG) && !indexed;
    const bool losslessEffort = o.lossless && (o.format == Fmt::WEBP || o.format == Fmt::AVIF || o.format == Fmt::JXL);

    // Search an integer parameter: if higherMeansLarger, a larger value typically makes a bigger
    // file (quality, colours). We want the highest such value whose size is <= target (or the
    // lowest if none fit). If higherMeansLarger is false (effort/compression), invert that.
    auto search = [&](int lo, int hi, bool higherMeansLarger, const std::function<void(ExportOptions &, int)> &setVal) {
        EncodeResult bestEnc = out.encoded;
        ExportOptions best = o;
        bool met = out.encoded.bytes > 0 && out.encoded.bytes <= targetBytes;
        if (met) {
            // Already under: raise quality / drop effort as far as we can while staying under.
        }
        int bestVal = higherMeansLarger ? lo : hi;
        // Evaluate the "smallest file" end first so we have a fallback.
        {
            ExportOptions t = o;
            setVal(t, higherMeansLarger ? lo : hi);
            if (stopped()) return;
            EncodeResult r = trial(t);
            if (r.error.isEmpty()) {
                best = t; bestEnc = r;
                bestVal = higherMeansLarger ? lo : hi;
                met = r.bytes > 0 && r.bytes <= targetBytes;
            }
        }
        int a = lo, b = hi;
        while (a <= b) {
            if (stopped()) return;
            const int mid = a + (b - a) / 2;
            ExportOptions t = o;
            setVal(t, mid);
            EncodeResult r = trial(t);
            if (!r.error.isEmpty()) { // skip this mid
                if (higherMeansLarger) b = mid - 1; else a = mid + 1;
                continue;
            }
            const bool fits = r.bytes > 0 && r.bytes <= targetBytes;
            if (fits) {
                met = true;
                // Prefer a fit with higher visual quality.
                const bool better = higherMeansLarger ? mid >= bestVal : mid <= bestVal;
                if (!bestEnc.bytes || bestEnc.bytes > targetBytes || better) {
                    best = t; bestEnc = r; bestVal = mid;
                }
                if (higherMeansLarger) a = mid + 1; else b = mid - 1;
            } else {
                if (higherMeansLarger) b = mid - 1; else a = mid + 1;
            }
        }
        out.options = best;
        out.encoded = bestEnc;
        out.metTarget = met;
    };

    if (lossyQuality) {
        search(1, 100, true, [](ExportOptions &t, int v) { t.quality = v; });
    } else if (indexed) {
        const int hi = qBound(2, o.colors > 0 ? o.colors : 256, 256);
        search(2, hi, true, [](ExportOptions &t, int v) { t.colors = v; });
        if (!out.metTarget && !stopped()) {
            // Colour count alone could not fit: drop dither and try again from the smallest palette.
            ExportOptions t = out.options;
            t.dither = false;
            t.ditherAmount = 0;
            t.tinyDither = 0;
            out.options = t;
            o = t;
            search(2, qBound(2, t.colors, 256), true, [](ExportOptions &x, int v) { x.colors = v; });
        }
    } else if (losslessPng) {
        search(0, 9, false, [](ExportOptions &t, int v) { t.pngCompression = v; });
    } else if (losslessEffort && o.format == Fmt::WEBP) {
        search(0, 6, false, [](ExportOptions &t, int v) { t.webpEffort = v; });
    } else if (losslessEffort && o.format == Fmt::JXL) {
        search(1, 9, false, [](ExportOptions &t, int v) { t.jxlEffort = v; });
    } else if (losslessEffort && o.format == Fmt::AVIF) {
        // avifSpeed: 0 is slowest / typically smaller.
        search(0, 10, true, [](ExportOptions &t, int v) { t.avifSpeed = v; });
    } else {
        out.metTarget = out.encoded.bytes > 0 && out.encoded.bytes <= targetBytes;
    }
    if (stopped()) out.cancelled = true;
    return out;
}

QString describeOptions(const ExportOptions &o) {
    QString s = ImageIO::formatName(o.format);
    if (ImageIO::hasLossless(o.format) && o.lossless) s += QStringLiteral(" lossless");
    else if (ImageIO::hasQuality(o.format)) s += QStringLiteral(" quality %1").arg(o.quality);
    auto palName = [](int p) {
        const QStringList names = quant::paletteNames();
        const int row = quant::paletteOrder().indexOf(p);
        return row >= 0 ? names[row] : names[0];
    };
    if (o.format == Fmt::GIF)
        s += QStringLiteral(" %1 colours %2%3").arg(o.colors).arg(palName(o.palette)).arg(o.dither ? QStringLiteral(", dither") : QString());
    if (o.format == Fmt::PNG || o.format == Fmt::APNG) {
        static const char *types[] = {"32", "24", "8", "greyscale"};
        s = QStringLiteral("PNG %1").arg(QLatin1String(types[qBound(0, o.pngType, 3)]));
        if (o.format == Fmt::APNG) s.prepend(QStringLiteral("Animated "));
        if (o.pngType == 2) {
            static const char *tr[] = {"no transparency", "index transparency", "alpha transparency"};
            s += QStringLiteral(" %1 colours %2%3, %4").arg(o.colors).arg(palName(o.palette)).arg(o.dither ? QStringLiteral(", dither") : QString())
                     .arg(QLatin1String(tr[qBound(0, o.pngTransparency, 2)]));
        } else if (o.png16) s += QStringLiteral(" 16-bit");
        if (o.pngInterlace) s += QStringLiteral(", interlaced");
        if (o.tinyPng) s += QStringLiteral(", Tiny (quality %1-%2)").arg(o.tinyQualityMin).arg(o.tinyQualityMax);
        if (o.pngEditable) s += QStringLiteral(", editable");
    }
    if (o.format == Fmt::JPEG) {
        static const char *enc[] = {"", " MozJPEG", " jpegli"};
        static const char *ss[] = {"4:4:4", "4:2:2", "4:2:0"};
        s += QLatin1String(enc[qBound(0, o.jpegEncoder, 2)]);
        if (o.jpegProgressive) s += QStringLiteral(", progressive");
        if (o.jpegOptimize) s += QStringLiteral(", optimised");
        s += QStringLiteral(", %1").arg(QLatin1String(ss[qBound(0, o.jpegSubsampling, 2)]));
        if (o.jpegSmoothing) s += QStringLiteral(", smoothing %1").arg(o.jpegSmoothing);
        if (o.jpegSharpenEdges) s += QStringLiteral(", sharpen edges");
        if (o.jpegStripMeta) s += o.jpegKeepIcc ? QStringLiteral(", metadata stripped (ICC kept)") : QStringLiteral(", metadata stripped");
    }
    if (o.format == Fmt::BMP) s += QStringLiteral(" %1-bit%2").arg(o.bmpBits).arg(o.bmpRle && (o.bmpBits == 4 || o.bmpBits == 8) ? QStringLiteral(" RLE") : QString());
    if (o.format == Fmt::ICO || o.format == Fmt::CUR || o.format == Fmt::ICNS) {
        const int mask = o.format == Fmt::ICNS ? o.icnsSizes : o.icoSizes;
        QStringList l;
        const QVector<int> sz = ImageIO::iconSizes();
        for (int i = 0; i < sz.size(); ++i) if (mask & (1 << i)) l << QString::number(sz[i]);
        s += QStringLiteral(" %1").arg(l.join(QLatin1Char('/')));
        if (o.format == Fmt::CUR) s += QStringLiteral(", hotspot %1,%2").arg(o.curHotX).arg(o.curHotY);
    }
    if (o.format == Fmt::DDS || o.format == Fmt::KTX2) {
        static const char *tf[] = {"RGBA8", "BC1", "BC3"};
        s += QStringLiteral(" %1%2").arg(QLatin1String(tf[qBound(0, o.ddsFormat, 2)])).arg(o.mipmaps ? QStringLiteral(", mipmaps") : QString());
        if (o.format == Fmt::KTX2 && o.ktxZstd) s += QStringLiteral(", zstd");
    }
    if (o.format == Fmt::EXR) {
        static const char *ec[] = {"uncompressed", "ZIP", "PIZ", "RLE", "ZIPS", "PXR24", "B44", "DWAA"};
        s += QStringLiteral(" %1 %2").arg(o.exrHalf ? QStringLiteral("half") : QStringLiteral("float")).arg(QLatin1String(ec[qBound(0, o.exrCompression, 7)]));
    }
    if (o.format == Fmt::PCX) s += o.pcx8 ? QStringLiteral(" 8-bit palette") : QStringLiteral(" 24-bit");
    if (o.format == Fmt::PPM || o.format == Fmt::PGM || o.format == Fmt::PBM) s += o.netpbmAscii ? QStringLiteral(" ASCII") : QStringLiteral(" binary");
    if (o.format == Fmt::TIFF) {
        static const char *comps[] = {"uncompressed", "LZW", "Deflate", "PackBits"};
        s += QStringLiteral(" %1, %2-bit%3").arg(QLatin1String(comps[qBound(0, o.tiffCompression, 3)])).arg(o.tiff16 ? 16 : 8)
                 .arg(o.alpha ? QStringLiteral(", alpha") : QString());
        if (o.animated) return s + QStringLiteral(" (multi-page)");
    }
    if (o.format == Fmt::TGA)
        s += QStringLiteral(" %1-bit %2%3").arg(o.alpha ? 32 : 24).arg(o.tgaRle ? QStringLiteral("RLE") : QStringLiteral("uncompressed"))
                 .arg(o.tgaTopLeft ? QStringLiteral(", top-left origin") : QString());
    if (o.format == Fmt::CHeader) {
        if (o.chStyle == 1) s += QStringLiteral(" XBM-style 1-bit");
        else if (o.chStyle == 3) s += QStringLiteral(" GIMP header (HEADER_PIXEL, RGB)");
        else if (o.chStyle == 2) {
            const int b = cheader::gimpBytesPerPixel(o.chPixel);
            s += QStringLiteral(" GIMP C source %1%2%3").arg(b == 4 ? QStringLiteral("RGBA") : b == 2 ? QStringLiteral("RGB565") : QStringLiteral("RGB"))
                     .arg(o.chRle ? QStringLiteral(", RLE") : QString()).arg(o.chGimpMacros ? QStringLiteral(", macros") : QString());
        } else {
            s += QLatin1Char(' ') + cheader::pixelNames().value(o.chPixel);
            if (o.chElement == 1 && cheader::wordBits(o.chPixel) && !o.chRle) s += QStringLiteral(", uint%1_t words").arg(cheader::wordBits(o.chPixel));
            if (cheader::isIndexed(o.chPixel)) s += QStringLiteral(", %1 colours").arg(o.chPixel == cheader::Indexed4 ? qMin(16, o.colors) : o.colors);
            if (o.chRle) s += QStringLiteral(", RLE");
        }
        if (o.chRawBin) s += QStringLiteral(" + .bin");
        if (!o.chStrip) s += QStringLiteral(", readable");
        if (o.animated && o.chStyle < 2) return s + QStringLiteral(" (all frames)");
        return s;
    }
    if (o.animated && ImageIO::canAnimate(o.format)) s += QStringLiteral(" (animated)");
    return s;
}

QString describeSize(qint64 bytes) {
    double kb = bytes / 1024.0;
    double sec = bytes * 8.0 / 56000.0;
    return QStringLiteral("%1K   %2 sec @56kbps").arg(kb, 0, 'f', kb < 100 ? 2 : 0).arg(qMax(1, qRound(sec)));
}

// ---------------------------------------------------------------- PreviewView
void PreviewView::setView(double z, QPointF c) {
    z = qBound(0.03, z, 64.0);
    // keep part of the document in view
    c.setX(qBound(-m_doc.width() * 0.5, c.x(), m_doc.width() * 1.5));
    c.setY(qBound(-m_doc.height() * 0.5, c.y(), m_doc.height() * 1.5));
    bool zc = !qFuzzyCompare(z, m_zoom);
    if (!zc && c == m_center) return;
    m_zoom = z;
    m_center = c;
    emit changed();
    if (zc) emit zoomChanged(z);
}

void PreviewView::zoomAt(double z, QPointF pt, QSize pane) {
    z = qBound(0.03, z, 64.0);
    if (pt.x() < 0 || pane.isEmpty()) { setView(z, m_center); return; }
    QPointF d = toDoc(pt, pane);
    // new centre so that d stays under pt
    QPointF c = d - (pt - QPointF(pane.width() / 2.0, pane.height() / 2.0)) / z;
    setView(z, c);
}

void PreviewView::zoomIn(QPointF pt, QSize pane) { zoomAt(nextZoomLevel(m_zoom, true), pt, pane); }
void PreviewView::zoomOut(QPointF pt, QSize pane) { zoomAt(nextZoomLevel(m_zoom, false), pt, pane); }

void PreviewView::zoomToRect(const QRectF &r, QSize pane) {
    if (r.width() < 1 || r.height() < 1 || pane.isEmpty()) return;
    setView(qMin(pane.width() / r.width(), pane.height() / r.height()), r.center());
}

void PreviewView::fit(QSize pane) {
    // fills the pane exactly on the limiting axis (no gap), plus the optional fit-margin pref
    const double margin = 2.0 * qMax(0, appState().prefs.fitMargin);
    double z = qMin((pane.width() - margin) / qMax(1, m_doc.width()), (pane.height() - margin) / qMax(1, m_doc.height()));
    if (z <= 0) z = 1;
    setView(z, QPointF(m_doc.width() / 2.0, m_doc.height() / 2.0));
}

void PreviewView::actualSize() { setView(1, m_center); }

// ---------------------------------------------------------------- PreviewPane
PreviewPane::PreviewPane(PreviewView *view, QWidget *parent) : QWidget(parent), m_view(view) {
    setMinimumSize(120, 90);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(view, &PreviewView::changed, this, qOverload<>(&QWidget::update));
    connect(&appState(), &AppState::toolChanged, this, &PreviewPane::updateCursor);
    updateCursor();
}

void PreviewPane::setResult(const QImage &img, const QString &caption) {
    m_img = img;
    m_caption = caption;
    update();
}

void PreviewPane::setSelected(bool on) {
    if (m_selected == on) return;
    m_selected = on;
    update();
}

void PreviewPane::updateCursor() {
    Tool t = appState().tool();
    if (m_space || t == Tool::Hand || m_drag == Drag::Pan) setCursor(m_drag == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    else if (t == Tool::Zoom) setCursor(Qt::CrossCursor);
    else setCursor(Qt::OpenHandCursor);
}

void PreviewPane::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), appState().prefs.pasteboardColor);
    QRect area(QPoint(0, 0), imageArea());
    p.setClipRect(area);
    if (!m_img.isNull()) {
        const double z = m_view->zoom();
        const QPointF origin = m_view->origin(area.size());
        // the export result always covers the whole document; size the target from the document
        // (fall back to the image size) and use the SAME snapped rect for checkerboard and image
        QSize ds = m_view->docSize();
        if (ds.isEmpty()) ds = m_img.size();
        const QRectF dst = snappedTarget(origin, QSizeF(ds), z, devicePixelRatioF());
        if (appState().prefs.canvasShadow) drawCanvasShadow(p, dst);
        if (m_img.hasAlphaChannel()) {
            // checkerboard for transparency, clipped to exactly the image target
            p.save();
            p.setClipRect(dst.intersected(QRectF(area)));
            p.fillRect(dst, Qt::white);
            const int cs = 8;
            const QRect vis = dst.toAlignedRect().intersected(area);
            const int ox = int(std::floor(dst.left())), oy = int(std::floor(dst.top()));
            for (int y = vis.top() - ((vis.top() - oy) % cs + cs) % cs; y <= vis.bottom(); y += cs)
                for (int x = vis.left() - ((vis.left() - ox) % cs + cs) % cs; x <= vis.right(); x += cs)
                    if ((((x - ox) / cs) + ((y - oy) / cs)) % 2) p.fillRect(x, y, cs, cs, QColor(0xCC, 0xCC, 0xCC));
            p.restore();
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, z < 1);
        p.drawImage(dst, m_img, QRectF(m_img.rect()));
        // 1px frame just outside the image target (never over image pixels)
        if (appState().prefs.canvasBorder) {
            p.setPen(canvasBorderColor());
            p.setBrush(Qt::NoBrush);
            p.drawRect(dst.adjusted(-0.5, -0.5, 0.5, 0.5));
        }
    }
    if (m_drag == Drag::ZoomRect) {
        p.setPen(QPen(Qt::black, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(m_zoomRect);
    }
    p.setClipping(false);
    // caption bar and highlight from the theme's palette, so the selected pane stands out in every theme
    const QPalette &pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor base = pal.color(QPalette::Window);
    auto mix = [](const QColor &a, const QColor &b, double t) {
        return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t, a.blueF() * (1 - t) + b.blueF() * t);
    };
    auto readable = [](const QColor &bg) { return bg.lightnessF() > 0.55 ? QColor(0x14, 0x14, 0x14) : QColor(0xF4, 0xF4, 0xF4); };
    QRect cap = rect().adjusted(0, height() - kCaption, 0, 0);
    const QColor capBg = m_selected ? mix(base, accent, 0.45) : base;
    p.fillRect(cap, capBg);
    p.setPen(mix(base, pal.color(QPalette::WindowText), 0.45));
    p.drawLine(cap.topLeft(), cap.topRight());
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.setPen(readable(capBg));
    QFont cf = font();
    if (m_selected) cf.setBold(true);
    p.setFont(cf);
    // the one-time tip shares the caption area (second line) so it never covers the image
    const QRect textRect = m_hint.isEmpty() ? cap.adjusted(6, 2, -6, -2) : QRect(cap.left() + 6, cap.top() + 1, cap.width() - 12, kCaption / 2 - 1);
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%").arg(qRound(m_view->zoom() * 100)));
    p.setFont(font());
    if (!m_hint.isEmpty()) {
        QRect hr(cap.left() + 1, cap.top() + kCaption / 2, cap.width() - 2, kCaption / 2 - 1);
        const QColor hb = mix(base, accent, 0.25);
        p.fillRect(hr, hb);
        p.setPen(readable(hb));
        QFont hf = font();
        hf.setItalic(true);
        p.setFont(hf);
        p.drawText(hr.adjusted(5, 0, -5, 0), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(m_hint, Qt::ElideRight, hr.width() - 10));
        p.setFont(font());
    }
    if (m_selected) { // a 3 px accent border round the selected (active) pane
        p.setPen(QPen(accent, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5));
    }
}

void PreviewPane::mousePressEvent(QMouseEvent *e) {
    setFocus(Qt::MouseFocusReason);
    m_last = m_press = e->position();
    const Tool t = appState().tool();
    // any left press selects the pane, whatever the tool or Space; pan / zoom carry on after it
    if (e->button() == Qt::LeftButton) emit clicked();
    if (e->button() == Qt::MiddleButton || m_space || t != Tool::Zoom) { m_drag = Drag::Pan; updateCursor(); return; }
    if (e->button() == Qt::LeftButton) { m_drag = Drag::ZoomRect; m_zoomRect = QRectF(m_press, m_press); }
}

void PreviewPane::focusInEvent(QFocusEvent *e) {
    QWidget::focusInEvent(e);
    if (e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason) emit clicked();
}

void PreviewPane::mouseMoveEvent(QMouseEvent *e) {
    if (m_drag == Drag::Pan) {
        QPointF d = e->position() - m_last;
        m_last = e->position();
        m_view->panBy(d); // panning a preview pans every synced pane
    } else if (m_drag == Drag::ZoomRect) {
        m_zoomRect = QRectF(m_press, e->position()).normalized();
        update();
    }
}

void PreviewPane::mouseReleaseEvent(QMouseEvent *e) {
    Drag d = m_drag;
    m_drag = Drag::None;
    if (d == Drag::ZoomRect) {
        if (m_zoomRect.width() > 4 && m_zoomRect.height() > 4) {
            QRectF dr(m_view->toDoc(m_zoomRect.topLeft(), imageArea()), m_view->toDoc(m_zoomRect.bottomRight(), imageArea()));
            m_view->zoomToRect(dr, imageArea());
        } else if (e->modifiers() & Qt::AltModifier) m_view->zoomOut(e->position(), imageArea());
        else m_view->zoomIn(e->position(), imageArea());
        update();
    }
    updateCursor();
}

void PreviewPane::mouseDoubleClickEvent(QMouseEvent *e) {
    // the second press of a double-click: the same as a press (selecting again is harmless)
    mousePressEvent(e);
}

void PreviewPane::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double f = e->angleDelta().y() > 0 ? 1.25 : 0.8;
        m_view->zoomAt(m_view->zoom() * f, e->position(), imageArea());
    } else {
        QPointF delta = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2.0 : QPointF(e->pixelDelta());
        if (e->modifiers() & Qt::ShiftModifier) delta = QPointF(delta.y(), delta.x());
        m_view->panBy(delta);
    }
    e->accept();
}

bool PreviewPane::handleZoomKey(QKeyEvent *e, PreviewView *v, QSize pane) {
    auto &sm = shortcuts::Manager::get();
    if (sm.matches(QStringLiteral("view.zoomin"), e)) { v->zoomIn(); return true; }
    if (sm.matches(QStringLiteral("view.zoomout"), e)) { v->zoomOut(); return true; }
    if (sm.matches(QStringLiteral("view.fit"), e)) { v->fit(pane); return true; }
    if (sm.matches(QStringLiteral("view.actual"), e)) { v->actualSize(); return true; }
    return false;
}

bool PreviewPane::event(QEvent *e) {
    // In a dialog (Export Preview) the pane's own zoom keys win over the main window's
    // View shortcuts, even when the window manager has not activated the dialog yet.
    if (e->type() == QEvent::ShortcutOverride && qobject_cast<QDialog *>(window())) {
        auto *k = static_cast<QKeyEvent *>(e);
        auto &sm = shortcuts::Manager::get();
        if (sm.matches(QStringLiteral("view.zoomin"), k) || sm.matches(QStringLiteral("view.zoomout"), k)
            || sm.matches(QStringLiteral("view.fit"), k) || sm.matches(QStringLiteral("view.actual"), k)
            || sm.matches(QStringLiteral("canvas.tempPan"), k)) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void PreviewPane::keyPressEvent(QKeyEvent *e) {
    auto &sm = shortcuts::Manager::get();
    if (sm.matches(QStringLiteral("canvas.tempPan"), e) && !e->isAutoRepeat()) { m_space = true; updateCursor(); return; }
    if (handleZoomKey(e, m_view, imageArea())) return;
    QWidget::keyPressEvent(e);
}

void PreviewPane::keyReleaseEvent(QKeyEvent *e) {
    auto &sm = shortcuts::Manager::get();
    bool tempPan = sm.matches(QStringLiteral("canvas.tempPan"), e);
    if (!tempPan) {
        for (const QKeySequence &k : sm.sequences(QStringLiteral("canvas.tempPan")))
            if (!k.isEmpty() && k[0].key() == e->key()) { tempPan = true; break; }
    }
    if (tempPan && !e->isAutoRepeat()) { m_space = false; updateCursor(); return; }
    QWidget::keyReleaseEvent(e);
}

// ---------------------------------------------------------------- DocumentView
DocumentView::DocumentView(Document *doc, QWidget *parent) : QWidget(parent), m_doc(doc) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_tabs = new QTabBar(this);
    m_tabs->setObjectName(QStringLiteral("ViewTabs"));
    for (const char *t : {"Original", "Preview", "2-Up", "4-Up"}) m_tabs->addTab(QString::fromLatin1(t));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    // the four view tabs always fit: never scroll or elide, always get their full size hint
    // (selected/unselected tabs share one font weight and horizontal padding, so the hint is stable)
    m_tabs->setUsesScrollButtons(false);
    m_tabs->setElideMode(Qt::ElideNone);
    m_tabs->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    auto *tabRow = new QWidget(this);
    tabRow->setObjectName(QStringLiteral("ViewTabRow"));
    // background from the theme stylesheet (#ViewTabRow)
    tabRow->setAttribute(Qt::WA_StyledBackground);
    auto *tl = new QHBoxLayout(tabRow);
    tl->setContentsMargins(6, 3, 6, 0);
    tl->addWidget(m_tabs);
    tl->addStretch();
    lay->addWidget(tabRow);

    m_stack = new QStackedWidget(this);
    // Original: the canvas with its rulers (View > Rulers)
    auto *orig = new QWidget(m_stack);
    auto *og = new QGridLayout(orig);
    og->setContentsMargins(0, 0, 0, 0);
    og->setSpacing(0);
    m_canvas = new Canvas(doc, orig);
    m_corner = new QWidget(orig);
    m_corner->setFixedSize(Ruler::kThickness, Ruler::kThickness);
    m_corner->setObjectName(QStringLiteral("RulerCorner"));   // themed (#RulerCorner)
    m_corner->setAttribute(Qt::WA_StyledBackground);
    m_hRuler = new Ruler(Qt::Horizontal, m_canvas, orig);
    m_vRuler = new Ruler(Qt::Vertical, m_canvas, orig);
    og->addWidget(m_corner, 0, 0);
    og->addWidget(m_hRuler, 0, 1);
    og->addWidget(m_vRuler, 1, 0);
    og->addWidget(m_canvas, 1, 1);
    auto showRulers = [this] {
        const bool on = appState().showRulers;
        m_corner->setVisible(on);
        m_hRuler->setVisible(on);
        m_vRuler->setVisible(on);
    };
    showRulers();
    connect(&appState(), &AppState::optionsChanged, this, showRulers);
    m_stack->addWidget(orig);
    m_view = new PreviewView(QSize(doc->s.width, doc->s.height), this);
    connect(m_canvas, &Canvas::zoomChanged, this, [this](double z) { if (m_mode == 0) emit zoomChanged(z); });
    connect(m_view, &PreviewView::zoomChanged, this, [this](double z) { if (m_mode != 0) emit zoomChanged(z); });
    m_single = new PreviewPane(m_view);
    m_stack->addWidget(m_single);
    auto *two = new QWidget;
    auto *tw = new QHBoxLayout(two);
    tw->setContentsMargins(0, 0, 0, 0);
    tw->setSpacing(2);
    for (auto &p : m_two) { p = new PreviewPane(m_view); tw->addWidget(p); }
    m_stack->addWidget(two);
    auto *four = new QWidget;
    auto *fw = new QGridLayout(four);
    fw->setContentsMargins(0, 0, 0, 0);
    fw->setSpacing(2);
    for (int i = 0; i < 4; ++i) {
        m_four[i] = new PreviewPane(m_view);
        fw->addWidget(m_four[i], i / 2, i % 2);
        if (i > 0) connect(m_four[i], &PreviewPane::clicked, this, [this, i] { selectPane(i - 1); });
    }
    m_stack->addWidget(four);
    lay->addWidget(m_stack, 1);

    connect(m_tabs, &QTabBar::currentChanged, this, &DocumentView::setMode);
    m_timer.setSingleShot(true);
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &DocumentView::refresh);
    connect(doc, &Document::changed, this, &DocumentView::scheduleRefresh);
    connect(doc, &Document::frameChanged, this, &DocumentView::scheduleRefresh);
}

DocumentView::~DocumentView() = default;

void DocumentView::setMode(int m) {
    m = qBound(0, m, 3);
    if (m_tabs->currentIndex() != m) m_tabs->setCurrentIndex(m);
    m_tabs->updateGeometry();
    if (m == m_mode && m_stack->currentIndex() == m) return;
    const int old = m_mode;
    m_mode = m;
    m_view->setDocSize(QSize(m_doc->s.width, m_doc->s.height));
    // Original and the previews share zoom and scroll position, as in Fireworks
    if (old == 0 && m != 0) m_view->setView(m_canvas->zoom(), m_canvas->docCenter());
    m_stack->setCurrentIndex(m);
    if (m == 0) {
        if (old != 0) m_canvas->setView(m_view->zoom(), m_view->center());
        m_canvas->setFocus();
    } else if (auto *p = previewPane(m, 0)) p->setFocus();
    emit modeChanged(m);
    emit zoomChanged(zoom());
    refresh();
}

void DocumentView::scheduleRefresh() {
    if (m_mode != 0) m_timer.start();
}

void DocumentView::refresh() {
    if (m_mode == 0) return;
    const ExportOptions &o = m_doc->exportOptions;
    const int f = m_doc->curFrame;
    auto caption = [](const ExportOptions &opt, const EncodeResult &r) {
        if (!r.error.isEmpty()) return QStringLiteral("%1\nError: %2").arg(describeOptions(opt), r.error);
        return QStringLiteral("%1\n%2%3%4").arg(describeOptions(opt), describeSize(r.bytes),
                                               r.frames > 1 ? QStringLiteral("   %1 frames").arg(r.frames) : QString(),
                                               r.originalBytes > 0 ? QStringLiteral("\n") + describeSaving(r) : QString());
    };
    QImage original = m_doc->render(f, true);
    QString origCap = QStringLiteral("Original: \"%1\"\n%2 x %3").arg(m_doc->title()).arg(m_doc->s.width).arg(m_doc->s.height);
    if (m_mode == 1) {
        EncodeResult r = encodePreview(*m_doc, o, f);
        m_single->setResult(r.image, caption(o, r));
    } else if (m_mode == 2) {
        m_two[0]->setResult(original, origCap);
        EncodeResult r = encodePreview(*m_doc, o, f);
        m_two[1]->setResult(r.image, caption(o, r));
    } else {
        // each pane owns its settings; they are derived only once (Document::ensurePaneOptions)
        m_doc->ensurePaneOptions();
        m_four[0]->setResult(original, origCap);
        m_four[0]->setHint(QSettings().value(QStringLiteral("hints/fourUpPanes")).toBool()
                               ? QString() : QStringLiteral("Click a pane to edit its settings in the Optimize panel"));
        for (int i = 0; i < Document::kPanes; ++i) {
            const ExportOptions po = m_doc->paneOption(i);
            EncodeResult r = encodePreview(*m_doc, po, f);
            m_four[i + 1]->setResult(r.image, caption(po, r));
        }
    }
    updateSelection();
}

void DocumentView::updateSelection() {
    m_single->setSelected(false);
    m_two[0]->setSelected(false);
    m_two[1]->setSelected(true);
    m_four[0]->setSelected(false);
    for (int i = 0; i < Document::kPanes; ++i) m_four[i + 1]->setSelected(i == m_doc->selectedPane);
}

void DocumentView::selectPane(int pane) {
    // the one-time tip has done its job once a pane has been picked
    if (!m_four[0]->hint().isEmpty()) {
        QSettings().setValue(QStringLiteral("hints/fourUpPanes"), true);
        m_four[0]->setHint(QString());
    }
    m_doc->ensurePaneOptions();
    if (pane == m_doc->selectedPane) { updateSelection(); return; }
    m_doc->selectPane(pane);
    updateSelection();
    emit paneSelected(pane);
}

void DocumentView::resetPaneOptions() {
    m_doc->resetPaneOptions();
    refresh();
    emit paneSelected(m_doc->selectedPane);
}

PreviewPane *DocumentView::previewPane(int mode, int index) const {
    if (mode == 1) return index == 0 ? m_single : nullptr;
    if (mode == 2) return index >= 0 && index < 2 ? m_two[index] : nullptr;
    if (mode == 3) return index >= 0 && index < 4 ? m_four[index] : nullptr;
    return nullptr;
}

QSize DocumentView::paneArea() const {
    PreviewPane *p = previewPane(m_mode, 0);
    return p ? p->imageArea() : size();
}

double DocumentView::zoom() const { return m_mode == 0 ? m_canvas->zoom() : m_view->zoom(); }
void DocumentView::zoomIn() { if (m_mode == 0) m_canvas->zoomIn(); else m_view->zoomIn(); }
void DocumentView::zoomOut() { if (m_mode == 0) m_canvas->zoomOut(); else m_view->zoomOut(); }
void DocumentView::setZoom(double z) { if (m_mode == 0) m_canvas->setZoom(z); else m_view->setView(z, m_view->center()); }
void DocumentView::fitSelection() {
    // the selected objects' bounds (or the pixel selection), else the whole document
    Document *d = m_doc;
    QRectF r = d->selectionBounds();
    if (r.isEmpty() && !d->pixelSelection.isEmpty()) r = d->pixelSelection.boundingRect();
    if (r.width() < 1 || r.height() < 1) { fit(); return; }
    if (m_mode == 0) m_canvas->zoomToRect(r);
    else m_view->zoomToRect(r.adjusted(-10 / m_view->zoom(), -10 / m_view->zoom(), 10 / m_view->zoom(), 10 / m_view->zoom()), paneArea());
}
void DocumentView::fit() { if (m_mode == 0) m_canvas->fitToWindow(); else m_view->fit(paneArea()); }
void DocumentView::actualSize() { if (m_mode == 0) m_canvas->actualSize(); else m_view->actualSize(); }
