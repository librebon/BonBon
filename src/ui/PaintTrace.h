#pragma once
// Opt-in real-X paint/event tracing. Set LW_PAINT_TRACE to a file path, or "1" for stderr.
// One line per record:
//   us kind ev_lat_ms dur_ms x y extra
// us is microseconds since the first event of the process, ev_lat_ms is
// (receive time - QInputEvent::timestamp) when known, dur_ms is the work
// inside the function, extra is free-form (grew, dirty, backlog, ...).

#include <QElapsedTimer>
#include <QFile>
#include <QInputEvent>
#include <QString>
#include <QTextStream>
#include <cstdio>
#include <cstdlib>

namespace painttrace {

inline bool enabled()
{
    static int once = 0;
    static bool on = false;
    if (!once) {
        once = 1;
        const char *e = getenv("LW_PAINT_TRACE");
        on = e && e[0] && !(e[0] == '0' && e[1] == 0);
    }
    return on;
}

inline QElapsedTimer &clock()
{
    static QElapsedTimer t;
    static bool started = false;
    if (!started) { t.start(); started = true; }
    return t;
}

inline QTextStream &out()
{
    static QFile f;
    static QTextStream ts;
    static bool opened = false;
    if (!opened) {
        opened = true;
        const char *e = getenv("LW_PAINT_TRACE");
        if (e && e[0] && e[0] != '1' && !(e[0] == '1' && e[1] == 0)) {
            f.setFileName(QString::fromLocal8Bit(e));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                fprintf(stderr, "LW_PAINT_TRACE: cannot write %s\n", e);
        } else {
            f.open(stdout, QIODevice::WriteOnly | QIODevice::Text);
        }
        ts.setDevice(&f);
        ts << "us kind ev_lat_ms dur_ms x y extra\n";
        ts.flush();
    }
    return ts;
}

inline qint64 nowUs() { return clock().nsecsElapsed() / 1000; }

// Moves received since the last paintEvent; incremented by onInput, cleared by onPaintEvent.
inline int &moveBacklog() { static int n = 0; return n; }
inline int &paintToCount() { static int n = 0; return n; }

inline void log(const char *kind, double evLatMs, double durMs, double x, double y, const QString &extra = QString())
{
    if (!enabled()) return;
    QTextStream &s = out();
    s << nowUs() << ' ' << kind << ' ' << evLatMs << ' ' << durMs << ' ' << x << ' ' << y;
    if (!extra.isEmpty()) s << ' ' << extra;
    s << '\n';
    s.flush();
}

inline double eventLatencyMs(const QInputEvent *e)
{
    if (!e) return -1;
    // QInputEvent::timestamp is milliseconds from an arbitrary origin. Compare two
    // events against our clock by storing the first pairing.
    static bool haveBase = false;
    static ulong ev0 = 0;
    static qint64 us0 = 0;
    const ulong ev = e->timestamp();
    const qint64 us = nowUs();
    if (!haveBase) { haveBase = true; ev0 = ev; us0 = us; return 0; }
    const qint64 evUs = qint64(ev - ev0) * 1000;
    const qint64 elapsed = us - us0;
    return (elapsed - evUs) / 1000.0;
}

struct Scope {
    const char *kind;
    double evLat, x, y;
    qint64 t0;
    QString extra;
    Scope(const char *k, double lat, double xx, double yy, const QString &ex = QString())
        : kind(k), evLat(lat), x(xx), y(yy), t0(nowUs()), extra(ex) {}
    ~Scope() { log(kind, evLat, (nowUs() - t0) / 1000.0, x, y, extra); }
};

} // namespace painttrace
