#include "ui/ScopesWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// The scope is drawn into this child (so the combo stays on top).
class ScopeArea : public QWidget {
public:
    explicit ScopeArea(const QImage* scope) : m_scope(scope) { setMinimumSize(160, 120); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 20));
        if (!m_scope->isNull())
            p.drawImage(QPoint(0, 0), *m_scope);
    }

private:
    const QImage* m_scope;
};

} // namespace

ScopesWidget::ScopesWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("videoScopes"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 0);
    bar->addWidget(new QLabel(tr("Scope")));
    m_kind = new QComboBox;
    m_kind->addItems(scopes::names());
    m_kind->setToolTip(tr("Waveform: brightness of each column of the picture (0-100%)\n"
                          "RGB Parade: red, green and blue the same way, side by side\n"
                          "Vectorscope: colour (angle = hue, distance = saturation), with the 75% bar targets and the skin tone line\n"
                          "Histogram: how many pixels at each level"));
    m_kind->setCurrentIndex(std::clamp(QSettings().value(QStringLiteral("scopes/kind"), 0).toInt(), 0, m_kind->count() - 1));
    bar->addWidget(m_kind, 1);
    layout->addLayout(bar);
    m_area = new ScopeArea(&m_scope);
    layout->addWidget(m_area, 1);
    connect(m_kind, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        QSettings().setValue(QStringLiteral("scopes/kind"), i);
        m_dirty = true;
        redraw();
    });
    m_timer.setInterval(66);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_dirty)
            redraw();
    });
    m_timer.start();
}

scopes::Kind ScopesWidget::kind() const
{
    return scopes::Kind(m_kind->currentIndex());
}

void ScopesWidget::setKind(scopes::Kind kind)
{
    m_kind->setCurrentIndex(int(kind));
}

void ScopesWidget::setPicture(const QImage& picture)
{
    m_picture = picture;
    m_dirty = true;
}

void ScopesWidget::redraw()
{
    m_dirty = false;
    if (!isVisible() || m_area->width() < 8 || m_area->height() < 8)
        return;
    m_scope = scopes::render(m_picture, kind(), m_area->size());
    m_area->update();
}

void ScopesWidget::paintEvent(QPaintEvent* e)
{
    QWidget::paintEvent(e);
}

void ScopesWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    m_dirty = true;
}

void ScopesWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    m_dirty = true;
}
