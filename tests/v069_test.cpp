// 0.3.32: live stroke layer on a real X server (xvfb-run + xcb). Mid-stroke
// ink must be unclipped, keep up with the pointer, and (after one worker
// cycle) match the committed stroke on a self-overlapping scribble.
// 0.3.33: retouch tools, Eraser, Preserve Transparency and non-Normal blend
// modes must also show their effect live mid-stroke (and match the commit).
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/BrushEngine.h"
#include "core/PixelEdit.h"
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/DocumentView.h"
#include "ui/Canvas.h"
#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QPainter>
#include <QtTest>
#include <cmath>

static const QString kShotDir = QStringLiteral("/workspace/review-0.3.32");

namespace {

void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs,
              Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos), b, bs, mods);
    QApplication::sendEvent(w, &ev);
}

void applyBrush(const QString &key, int size)
{
    appState().setTool(Tool::Brush);
    QCoreApplication::processEvents();
    const stroke::Params sp = stroke::paramsFor(key);
    const QStringList cats = brush::categoryNames();
    int ci = cats.indexOf(sp.category);
    if (ci < 0) ci = 0;
    appState().brushCategory = ci;
    appState().brushTip = brush::tipIndex(brush::Category(ci), sp.name);
    appState().brushSize = size;
    appState().strokeCustom = false;
    appState().strokeKind = key;
    appState().toolOpacity = 100;
    appState().brushTexture.clear();
    appState().pressureSize = false;
    appState().pressureOpacity = false;
    appState().pressureFlow = false;
    appState().tiltAngle = false;
    appState().setStroke(QColor(20, 80, 200));
    appState().strokeTarget = AppState::ExpandSelected;
    QCoreApplication::processEvents();
}

static int rgbDist(QRgb a, QRgb b)
{
    return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
}

int missingPaint(const QImage &ref, const QImage &got, QRgb bg = qRgb(255, 255, 255), int minDist = 18)
{
    if (ref.size() != got.size()) return ref.width() * ref.height() + 1;
    int n = 0;
    for (int y = 0; y < ref.height(); ++y) {
        const auto *pr = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *pg = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < ref.width(); ++x)
            if (rgbDist(pr[x], bg) >= minDist && rgbDist(pg[x], bg) < minDist / 2) ++n;
    }
    return n;
}

int paintPastRect(const QImage &frame, const QRectF &oldDocRect, QRgb bg = qRgb(255, 255, 255), int minDist = 18)
{
    const QRect old = oldDocRect.toAlignedRect();
    int n = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const auto *p = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x)
            if (!old.contains(x, y) && rgbDist(p[x], bg) >= minDist) ++n;
    }
    return n;
}

int changedFrom(const QImage &a, const QImage &b, int minDist = 18)
{
    if (a.size() != b.size()) return a.width() * a.height() + 1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (rgbDist(pa[x], pb[x]) >= minDist) ++n;
    }
    return n;
}

// Pixels that changed in `ref` vs `pre` but are still the pre-stroke colour in `got`.
int effectMissing(const QImage &pre, const QImage &ref, const QImage &got, int minDist = 18)
{
    if (pre.size() != ref.size() || pre.size() != got.size())
        return pre.width() * pre.height() + 1;
    int n = 0;
    for (int y = 0; y < pre.height(); ++y) {
        const auto *pp = reinterpret_cast<const QRgb *>(pre.constScanLine(y));
        const auto *pr = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *pg = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < pre.width(); ++x)
            if (rgbDist(pr[x], pp[x]) >= minDist && rgbDist(pg[x], pp[x]) < minDist / 2) ++n;
    }
    return n;
}

QImage patternImage(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int y = 0; y < h; y += 20) {
        for (int x = 0; x < w; x += 20) {
            const int k = ((x / 20) + (y / 20)) % 3;
            const QColor c = k == 0 ? QColor(240, 220, 40)
                           : k == 1 ? QColor(40, 200, 220)
                                    : QColor(220, 60, 180);
            p.fillRect(x, y, 20, 20, c);
        }
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(220, 30, 30));
    for (int i = 0; i < 6; ++i)
        p.drawEllipse(QPoint(50 + i * 50, 40), 18, 18);
    p.fillRect(20, h / 2, w - 40, 18, QColor(30, 180, 50));
    p.end();
    return img;
}

QVector<QPointF> scribblePts(QPointF c)
{
    QVector<QPointF> pts;
    for (int i = 0; i < 240; ++i) {
        const double ang = i / 18.0;
        const double rad = 20 + (i % 80);
        pts << QPointF(c.x() + rad * std::cos(ang), c.y() + rad * std::sin(ang) * 0.7);
    }
    return pts;
}

void savePng(const QImage &img, const QString &name)
{
    if (img.isNull()) return;
    QDir().mkpath(kShotDir);
    img.save(kShotDir + QLatin1Char('/') + name);
}

} // namespace

class V069Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        Prefs p;
        p.enableWebFeatures = false;
        appState().setPrefs(p);
        appState().setTool(Tool::Brush);
        appState().strokeTarget = AppState::ExpandSelected;
        appState().preserveTransparency = false;
        appState().toolBlend = 0;
        appState().toolOpacity = 100;
        appState().toolIntensity = 80;
        appState().brushSize = 28;
        appState().brushEdge = 40;
        QCoreApplication::processEvents();
    }

    void midStrokeUnclippedAndMatchesFinal()
    {
        MainWindow win;
        win.resize(1100, 900);
        win.show();
        QCoreApplication::processEvents();

        const QStringList keys{
            QStringLiteral("basic.hard"),
            QStringLiteral("unnatural.alienPaint"),
            QStringLiteral("unnatural.3dGlow"),
            QStringLiteral("lw.inkWash"),
            QStringLiteral("charcoal.soft")
        };
        const QRectF bm(180, 200, 28, 28);
        int fail = 0;
        QString msg;
        QString report;

        for (const QString &key : keys) {
            auto *doc = Document::createNew(900, 700, Qt::white);
            DocumentView *v = win.addDocument(doc);
            Canvas *cv = v->canvas();
            QCoreApplication::processEvents();
            cv->setZoom(1.0, QPointF(0, 0));
            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = bm;
            o.image = QImage(28, 28, QImage::Format_ARGB32_Premultiplied);
            o.image.fill(Qt::transparent);
            o.id = doc->newId();
            doc->addObject(o);
            doc->setSelection({o.id});
            doc->commit(QStringLiteral("bitmap"));
            applyBrush(key, 22);

            QVector<QPointF> pts = scribblePts(QPointF(260, 280));
            pts.prepend(QPointF(188, 208)); // start inside the tiny bitmap
            pts.append(QPointF(520, 400));  // jump well past the old bounds

            sendMouse(cv, QEvent::MouseButtonPress, cv->toView(pts.first()), Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i < pts.size(); ++i)
                sendMouse(cv, QEvent::MouseMove, cv->toView(pts[i]), Qt::NoButton, Qt::LeftButton);
            // Let the 16 ms paint-frame timer present mid-stroke ink before the grab.
            QTest::qWait(30);
            QCoreApplication::processEvents();

            const QImage midGrab = cv->grab().toImage();
            const int midPast = paintPastRect(midGrab, bm);
            cv->waitLiveStroke();
            QCoreApplication::processEvents();
            const QImage liveGrab = cv->grab().toImage();
            const int livePast = paintPastRect(liveGrab, bm);

            sendMouse(cv, QEvent::MouseButtonRelease, cv->toView(pts.last()), Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            const QImage finalGrab = cv->grab().toImage();
            const int finalPast = paintPastRect(finalGrab, bm);
            const int miss = missingPaint(finalGrab, liveGrab);

            savePng(midGrab, QStringLiteral("v069-mid-%1.png").arg(key));
            savePng(liveGrab, QStringLiteral("v069-live-%1.png").arg(key));
            savePng(finalGrab, QStringLiteral("v069-final-%1.png").arg(key));
            report += QStringLiteral("%1 midPast=%2 livePast=%3 finalPast=%4 liveVsFinal=%5\n")
                          .arg(key).arg(midPast).arg(livePast).arg(finalPast).arg(miss);

            if (midPast < 40) {
                ++fail;
                msg += key + QStringLiteral(" mid-stroke clipped (past=%1); ").arg(midPast);
            }
            if (finalPast < 40) {
                ++fail;
                msg += key + QStringLiteral(" commit clipped (past=%1); ").arg(finalPast);
            }
            // After one worker cycle the live layer should match the commit.
            // Widget grabs also include the size-circle cursor (live only) and
            // a 1-px AA/selection fringe around the stroke (~2-2.6k px), so
            // allow a little headroom above that.
            if (miss > 4000) {
                ++fail;
                msg += key + QStringLiteral(" live-vs-final %1 px; ").arg(miss);
            }
            Q_UNUSED(v);
        }
        QDir().mkpath(kShotDir);
        QFile f(kShotDir + QStringLiteral("/v069-report.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
        QVERIFY2(fail == 0, qPrintable(msg + QLatin1Char('\n') + report));
    }

    void midStrokeRetouchEraserAndBlendLive()
    {
        static const QString kOut = QStringLiteral("/workspace/review-0.3.33");
        QDir().mkpath(kOut);

        MainWindow win;
        win.resize(1100, 900);
        win.show();
        QCoreApplication::processEvents();

        struct Case {
            const char *name;
            Tool tool;
            int blend;
            bool preserveTrans;
            bool needsStamp;
            bool customPreserveImg;
        };
        const Case cases[] = {
            {"smudge", Tool::Smudge, 0, false, false, false},
            {"blur", Tool::Blur, 0, false, false, false},
            {"sharpen", Tool::Sharpen, 0, false, false, false},
            {"dodge", Tool::Dodge, 0, false, false, false},
            {"burn", Tool::Burn, 0, false, false, false},
            {"rubberstamp", Tool::RubberStamp, 0, false, true, false},
            {"healing", Tool::Healing, 0, false, true, false},
            {"spothealing", Tool::SpotHealing, 0, false, false, false},
            {"replacecolor", Tool::ReplaceColor, 0, false, false, false},
            {"redeye", Tool::RedEye, 0, false, false, false},
            {"eraser", Tool::Eraser, 0, false, false, false},
            {"preserve-trans", Tool::Brush, 0, true, false, true},
            {"blend-multiply", Tool::Brush, int(BlendMode::Multiply), false, false, false},
        };

        int fail = 0;
        QString msg;
        QString report;

        for (const Case &c : cases) {
            auto *doc = Document::createNew(700, 500, Qt::white);
            DocumentView *v = win.addDocument(doc);
            Canvas *cv = v->canvas();
            QCoreApplication::processEvents();
            cv->setZoom(1.0, QPointF(0, 0));
            cv->pinToOrigin();

            const QRectF bm(40, 40, 400, 260);
            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = bm;
            if (c.customPreserveImg) {
                o.image = QImage(400, 260, QImage::Format_ARGB32_Premultiplied);
                o.image.fill(Qt::transparent);
                QPainter p(&o.image);
                p.fillRect(0, 0, 200, 260, QColor(240, 220, 40));
                p.end();
            } else {
                o.image = patternImage(400, 260);
            }
            o.id = doc->newId();
            doc->addObject(o);
            doc->setSelection({o.id});
            doc->commit(QStringLiteral("bitmap"));

            applyBrush(QStringLiteral("basic.hard"), 28);
            appState().setTool(c.tool);
            // setTool restores per-tool slots; apply options after the switch.
            appState().preserveTransparency = c.preserveTrans;
            appState().toolBlend = c.blend;
            appState().toolOpacity = 100;
            appState().toolIntensity = 80;
            appState().brushSize = 28;
            appState().brushEdge = 40;
            appState().rcFrom = QColor(220, 30, 30);
            appState().rcTo = QColor(0, 220, 80);
            appState().rcFromImage = false;
            appState().rcTolerance = 40;
            appState().reTolerance = 80;
            appState().setStroke(QColor(20, 40, 200));
            QCoreApplication::processEvents();

            const QPointF src(90, 80);   // a red disk
            // Replace Color / Red Eye act on the red disks along y=40.
            const QPointF a = (c.tool == Tool::ReplaceColor || c.tool == Tool::RedEye)
                                  ? QPointF(50, 80) : QPointF(80, 160);
            const QPointF b = (c.tool == Tool::ReplaceColor || c.tool == Tool::RedEye)
                                  ? QPointF(340, 80) : QPointF(340, 200);
            QVector<QPointF> pts;
            pts << a;
            for (int i = 1; i <= 24; ++i) {
                const double t = i / 24.0;
                const double wiggle = ((i % 2) ? 10.0 : -10.0);
                pts << QPointF(a.x() + (b.x() - a.x()) * t,
                               a.y() + (b.y() - a.y()) * t + wiggle);
            }

            if (c.needsStamp) {
                sendMouse(cv, QEvent::MouseButtonPress, cv->toView(src), Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
                sendMouse(cv, QEvent::MouseButtonRelease, cv->toView(src), Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
                QCoreApplication::processEvents();
            }

            const QImage preGrab = cv->grab().toImage();
            sendMouse(cv, QEvent::MouseButtonPress, cv->toView(pts.first()), Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i < pts.size(); ++i)
                sendMouse(cv, QEvent::MouseMove, cv->toView(pts[i]), Qt::NoButton, Qt::LeftButton);
            QTest::qWait(30);
            QCoreApplication::processEvents();
            const QImage midGrab = cv->grab().toImage();

            sendMouse(cv, QEvent::MouseButtonRelease, cv->toView(pts.last()), Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            const QImage finalGrab = cv->grab().toImage();

            const int midDelta = changedFrom(preGrab, midGrab);
            const int finalDelta = changedFrom(preGrab, finalGrab);
            const int missing = effectMissing(preGrab, finalGrab, midGrab);
            const QString stem = QString::fromUtf8(c.name);
            midGrab.save(kOut + QStringLiteral("/v069-retouch-mid-%1.png").arg(stem));
            finalGrab.save(kOut + QStringLiteral("/v069-retouch-final-%1.png").arg(stem));
            report += QStringLiteral("%1 midDelta=%2 finalDelta=%3 missing=%4\n")
                          .arg(stem).arg(midDelta).arg(finalDelta).arg(missing);

            const int midFloor = qMax(500, finalDelta / 3);
            if (finalDelta < 400) {
                ++fail;
                msg += stem + QStringLiteral(" commit had almost no effect (finalDelta=%1); ").arg(finalDelta);
            }
            if (midDelta < midFloor) {
                ++fail;
                msg += stem + QStringLiteral(" mid-stroke not live (midDelta=%1 floor=%2 finalDelta=%3); ")
                                  .arg(midDelta).arg(midFloor).arg(finalDelta);
            }
            // Cursor / selection fringe is a few thousand px; the whole stroke
            // must already be visible before release.
            if (missing > 4000 && missing > finalDelta / 5) {
                ++fail;
                msg += stem + QStringLiteral(" mid missing %1 px of the commit; ").arg(missing);
            }
            Q_UNUSED(v);
            appState().preserveTransparency = false;
            appState().toolBlend = 0;
        }

        QFile f(kOut + QStringLiteral("/v069-retouch-report.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
        QVERIFY2(fail == 0, qPrintable(msg + QLatin1Char('\n') + report));
    }
};


QTEST_MAIN(V069Test)
#include "v069_test.moc"
