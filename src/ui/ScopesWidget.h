#pragma once

#include "core/Scopes.h"

#include <QImage>
#include <QTimer>
#include <QWidget>

class QComboBox;

// The editor's video scopes, beside the preview: waveform, RGB parade,
// vectorscope or histogram of the picture the preview shows, redrawn up to
// 15 times a second while visible.
class ScopesWidget : public QWidget {
    Q_OBJECT
public:
    explicit ScopesWidget(QWidget* parent = nullptr);

    void setPicture(const QImage& picture);
    scopes::Kind kind() const;
    void setKind(scopes::Kind kind);
    // What is drawn now (tests).
    QImage scopeImage() const { return m_scope; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void redraw();

    QComboBox* m_kind;
    QWidget* m_area;
    QImage m_picture;
    QImage m_scope;
    bool m_dirty = false;
    QTimer m_timer;
};
