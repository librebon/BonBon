#pragma once

class QCoreApplication;
class QWidget;

// Keep a control's descriptive label (form buddy, grid/box neighbour, or
// QLabel::buddy) in the same enabled state as the control, and grey a
// section heading when every interactive child under it is disabled.
namespace uiutil {

void setEnabledWithLabel(QWidget *w, bool on);
void syncLabelsFor(QWidget *w);
void syncSectionLabels(QWidget *root);
void installEnableLabelFilter(QCoreApplication *app = nullptr);

} // namespace uiutil
