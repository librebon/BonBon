#pragma once
#include <QComboBox>
#include <QFontComboBox>
#include <QPointer>
#include <QTimer>
class QStandardItemModel;

// Measure and size a font-picking combo's popup to fit the widest rendered item,
// at least 2.3× the combo width, capped to the screen. The combo's own width is unchanged.
int widenFontPopup(QComboBox *combo);
constexpr double kFontPopupMinFactor = 2.3;

// QFontComboBox whose popup is sized in showPopup (Qt often resets view width there).
class WideFontComboBox : public QFontComboBox {
    Q_OBJECT
public:
    explicit WideFontComboBox(QWidget *parent = nullptr);
    void showPopup() override;
    void refreshFontSources(); // filter families the Font sources pref does not allow
    void setCurrentFont(const QFont &font); // also keeps an excluded family in the list
    // Select by stored family name (document / PI). Inserts the name if it is
    // not in the current source list, and marks truly missing faces instead of
    // falling back to a different installed font.
    void setCurrentFamily(const QString &family);
    QString currentFamily() const;
    QFont currentFont() const; // family follows currentFamily(), not a fallback face
    bool event(QEvent *e) override;
    static constexpr double kMinFactor = kFontPopupMinFactor;
private:
    QStandardItemModel *m_list = nullptr;
    QString m_stickyFamily; // last family we or the user chose; survives a theme/app-font reset
};

// Fill a plain combo with fontSourceFamilies() (optional leading empty item).
void fillFontSourceCombo(QComboBox *combo, bool includeEmpty = false);

// Plain QComboBox (e.g. Find & Replace font lists) with the same popup sizing.
class WidePopupComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit WidePopupComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}
    void showPopup() override {
        QComboBox::showPopup();
        widenFontPopup(this);
        widenFontPopup(this);
        QPointer<QComboBox> self(this);
        auto reclamp = [self] { if (self) widenFontPopup(self); };
        QTimer::singleShot(0, this, reclamp);
        QTimer::singleShot(1, this, reclamp);
    }
};
