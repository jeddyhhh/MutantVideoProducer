#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleOption>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleFactory>
#include <QWidget>
#include <map>

namespace theme {

namespace {

// clang-format off
const std::vector<Colors> kThemes = {
    // id, name, dark,
    // window, panel, base, raised, hover, checked, deep, line,   text, bright, dim, faint,   accent, onAccent,
    // timelineBg, trackBg, trackBgAlt, trackSelected, headerBg, headerSelected,   ruler, markerBar, corner, timecode, field,
    // videoClip, audioClip, clipSelected, clipText, cursor, playhead, region, waveform,   good, warn, bad, info, cue, key,   ink
    {"classic", "Classic", true,
     QColor(0x2d2d30), QColor(0x252527), QColor(0x252527), QColor(0x3a3a3e), QColor(0x46464c), QColor(0x4a5a74), QColor(0x1c1c1e), QColor(0x3a3a42),   QColor(0xdcdcdc), QColor(0xe6e6e8), QColor(0x9a9aa0), QColor(0x808086),   QColor(0x4a90e2), QColor(0xffffff),
     QColor(0x262628), QColor(0x3a3a3d), QColor(0x353538), QColor(0x464a52), QColor(0x303033), QColor(0x3c4048),   QColor(0x222224), QColor(0x2e2e32), QColor(0x1a1a1c), QColor(0x7fd8ff), QColor(0x1e1e20),
     QColor(0x3f5f8f), QColor(0x3f7a55), QColor(0xffa52e), QColor(0xdcdcdc), QColor(0xffffff), QColor(0xff5540), QColor(0x5a8cd8), QColor(0xa8e6b4),   QColor(0x4cd064), QColor(0xffd400), QColor(0xff5050), QColor(0x7fb8ff), QColor(0xc090ff), QColor(0xffb347),   QColor(0xffffff)},
    {"neon", "Neon Grid", true,
     QColor(0x120a20), QColor(0x0b0716), QColor(0x0e0919), QColor(0x23163b), QColor(0x2f1e50), QColor(0x4a1f6e), QColor(0x06040c), QColor(0x2a1b45),   QColor(0xe8e0ff), QColor(0xffffff), QColor(0x9a8cc4), QColor(0x6f6296),   QColor(0xff2bd6), QColor(0xffffff),
     QColor(0x0b0716), QColor(0x1a1030), QColor(0x160d2a), QColor(0x2b1748), QColor(0x140c26), QColor(0x2b1748),   QColor(0x08050f), QColor(0x120a20), QColor(0x06040c), QColor(0x00f0ff), QColor(0x0a0614),
     QColor(0x0e6f8a), QColor(0x8f1a78), QColor(0xf9f871), QColor(0xffffff), QColor(0x00f0ff), QColor(0xff2bd6), QColor(0x7a2bff), QColor(0xff9ef0),   QColor(0x39ff9c), QColor(0xf9f871), QColor(0xff3b6b), QColor(0x00f0ff), QColor(0xc77dff), QColor(0xf9f871),   QColor(0xffffff)},
    {"nightcity", "Night City", true,
     QColor(0x101114), QColor(0x08090b), QColor(0x0b0c0f), QColor(0x1c1e23), QColor(0x2a2d33), QColor(0x4a4510), QColor(0x040405), QColor(0x23262d),   QColor(0xe9e7d8), QColor(0xffffff), QColor(0x8a8d94), QColor(0x666970),   QColor(0xfcee0a), QColor(Qt::black),
     QColor(0x08090b), QColor(0x15171b), QColor(0x121418), QColor(0x2a2810), QColor(0x101216), QColor(0x2a2810),   QColor(0x050506), QColor(0x0e0f12), QColor(Qt::black), QColor(0xfcee0a), QColor(0x060708),
     QColor(0x087e8b), QColor(0xa3002a), QColor(0xfcee0a), QColor(0xffffff), QColor(0x02d7f2), QColor(0xff003c), QColor(0x02a8bd), QColor(0xffb3c1),   QColor(0x00ff9f), QColor(0xfcee0a), QColor(0xff003c), QColor(0x02d7f2), QColor(0xff4ff0), QColor(0xfcee0a),   QColor(0xffffff)},
    {"synthwave", "Synthwave Sunset", true,
     QColor(0x1f1238), QColor(0x170d2e), QColor(0x1a1033), QColor(0x2c1a52), QColor(0x3a2466), QColor(0x5a2a6a), QColor(0x0f0820), QColor(0x3a2566),   QColor(0xf5e9ff), QColor(0xffffff), QColor(0xa48cc9), QColor(0x7d68a3),   QColor(0xff8a3d), QColor(0x170d2e),
     QColor(0x170d2e), QColor(0x26174a), QColor(0x221443), QColor(0x3d2366), QColor(0x1e1240), QColor(0x3d2366),   QColor(0x120a26), QColor(0x1d1138), QColor(0x0f0820), QColor(0xffd36b), QColor(0x140b28),
     QColor(0xb03a7a), QColor(0xc4622a), QColor(0xffd36b), QColor(0xffffff), QColor(0xffd36b), QColor(0xff4f9a), QColor(0x7a4fd0), QColor(0xffd7b0),   QColor(0x6dffb0), QColor(0xffd36b), QColor(0xff4f6a), QColor(0x7fd4ff), QColor(0xd08cff), QColor(0xffd36b),   QColor(0xffffff)},
    {"terminal", "Terminal", true,
     QColor(0x050906), QColor(0x020403), QColor(0x030604), QColor(0x0c1a10), QColor(0x12301c), QColor(0x154a26), QColor(Qt::black), QColor(0x123320),   QColor(0xb8ffcf), QColor(0xe0ffe8), QColor(0x4e9c68), QColor(0x3a7a50),   QColor(0x33ff66), QColor(0x001a08),
     QColor(0x020403), QColor(0x08140c), QColor(0x061009), QColor(0x113a1f), QColor(0x060e09), QColor(0x113a1f),   QColor(Qt::black), QColor(0x050b07), QColor(Qt::black), QColor(0x33ff66), QColor(0x010302),
     QColor(0x16632f), QColor(0x7a5600), QColor(0xffb000), QColor(0xd8ffe2), QColor(0xffb000), QColor(0xff4040), QColor(0x1fa64a), QColor(0xffd27a),   QColor(0x33ff66), QColor(0xffb000), QColor(0xff4040), QColor(0x66ffcc), QColor(0x66ffcc), QColor(0xffb000),   QColor(0xffffff)},
    {"arctic", "Arctic", true,
     QColor(0x2e3440), QColor(0x2a2f3a), QColor(0x2e3440), QColor(0x3b4252), QColor(0x434c5e), QColor(0x4c566a), QColor(0x1f232b), QColor(0x3b4252),   QColor(0xe5e9f0), QColor(0xeceff4), QColor(0x9aa3b5), QColor(0x7b8496),   QColor(0x88c0d0), QColor(0x2e3440),
     QColor(0x262b35), QColor(0x353c4a), QColor(0x313745), QColor(0x434c5e), QColor(0x2e3440), QColor(0x434c5e),   QColor(0x22262e), QColor(0x2a2f3a), QColor(0x1f232b), QColor(0x88c0d0), QColor(0x232831),
     QColor(0x5e81ac), QColor(0x5f8a50), QColor(0xebcb8b), QColor(0xeceff4), QColor(0xeceff4), QColor(0xbf616a), QColor(0x81a1c1), QColor(0xcfe3bf),   QColor(0xa3be8c), QColor(0xebcb8b), QColor(0xbf616a), QColor(0x88c0d0), QColor(0xb48ead), QColor(0xd08770),   QColor(0xffffff)},
    {"studio", "Studio Light", false,
     QColor(0xe9e9ec), QColor(0xf7f7f9), QColor(0xffffff), QColor(0xdcdce2), QColor(0xd0d0d8), QColor(0xb9cdf5), QColor(0xb4b4bc), QColor(0xc8c8d0),   QColor(0x1f1f24), QColor(Qt::black), QColor(0x5f5f68), QColor(0x7a7a84),   QColor(0x2f6fed), QColor(0xffffff),
     QColor(0xdcdce0), QColor(0xececf0), QColor(0xe4e4e9), QColor(0xcfd9ee), QColor(0xe2e2e7), QColor(0xcfd9ee),   QColor(0xf4f4f6), QColor(0xe6e6ea), QColor(0xf4f4f6), QColor(0x2f6fed), QColor(0xd4d4da),
     QColor(0x4b78c8), QColor(0x2f9a6a), QColor(0xff8a00), QColor(0xffffff), QColor(0x1f1f24), QColor(0xe0245e), QColor(0x7aa2f0), QColor(0xd8f5e6),   QColor(0x1f9d55), QColor(0xb07800), QColor(0xd0213f), QColor(0x1f6fd0), QColor(0x8040d0), QColor(0xd97a00),   QColor(Qt::black)},
};
// clang-format on

size_t g_current = 0;

// "@dim" -> the theme's colour of that name.
const std::map<QString, QColor Colors::*>& names()
{
    static const std::map<QString, QColor Colors::*> m = {
        {QStringLiteral("window"), &Colors::window}, {QStringLiteral("panel"), &Colors::panel}, {QStringLiteral("base"), &Colors::base},
        {QStringLiteral("raised"), &Colors::raised}, {QStringLiteral("hover"), &Colors::hover}, {QStringLiteral("checked"), &Colors::checked},
        {QStringLiteral("deep"), &Colors::deep}, {QStringLiteral("line"), &Colors::line}, {QStringLiteral("text"), &Colors::text},
        {QStringLiteral("bright"), &Colors::bright}, {QStringLiteral("dim"), &Colors::dim}, {QStringLiteral("faint"), &Colors::faint},
        {QStringLiteral("accent"), &Colors::accent}, {QStringLiteral("onAccent"), &Colors::onAccent}, {QStringLiteral("field"), &Colors::field},
        {QStringLiteral("good"), &Colors::good}, {QStringLiteral("warn"), &Colors::warn}, {QStringLiteral("bad"), &Colors::bad},
        {QStringLiteral("info"), &Colors::info}, {QStringLiteral("cue"), &Colors::cue}, {QStringLiteral("key"), &Colors::key},
        {QStringLiteral("clipSelected"), &Colors::clipSelected}, {QStringLiteral("timecode"), &Colors::timecode}};
    return m;
}

// Fusion, with check boxes in the theme's colours (Fusion's own outline is a
// darker shade of the window: lost on dark themes).
class Style : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p, const QWidget* w) const override
    {
        if (pe != PE_IndicatorCheckBox && pe != PE_IndicatorItemViewItemCheck) {
            QProxyStyle::drawPrimitive(pe, opt, p, w);
            return;
        }
        const Colors& t = c();
        const bool on = opt->state & State_On, partial = opt->state & State_NoChange, enabled = opt->state & State_Enabled;
        const int side = std::min(opt->rect.width(), opt->rect.height()) - 2;
        const QRectF r(opt->rect.x() + (opt->rect.width() - side) / 2.0 + 0.5, opt->rect.y() + (opt->rect.height() - side) / 2.0 + 0.5, side - 1, side - 1);
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(QPen(!enabled ? t.faint : (opt->state & State_MouseOver) ? t.accent : t.dim, 1.2));
        p->setBrush((on || partial) && enabled ? t.accent : t.base);
        p->drawRoundedRect(r, 2, 2);
        p->setPen(QPen(enabled ? t.onAccent : t.faint, std::max(1.5, side / 7.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (on) {
            const QPointF a(r.left() + r.width() * 0.22, r.top() + r.height() * 0.52), b(r.left() + r.width() * 0.42, r.top() + r.height() * 0.72),
                c2(r.left() + r.width() * 0.78, r.top() + r.height() * 0.3);
            p->drawPolyline(QList<QPointF>{a, b, c2});
        } else if (partial) {
            p->drawLine(QPointF(r.left() + r.width() * 0.25, r.center().y()), QPointF(r.right() - r.width() * 0.25, r.center().y()));
        }
        p->restore();
    }
};

const char* kSheetProperty = "themeSheet";
const char* kIconProperty = "themeIcon";

} // namespace

const std::vector<Colors>& themes()
{
    return kThemes;
}

const Colors& c()
{
    return kThemes[g_current];
}

QString css(const QString& sheet)
{
    static const QRegularExpression token(QStringLiteral("@([A-Za-z]+)"));
    QString out;
    qsizetype last = 0;
    for (auto m = token.globalMatch(sheet); m.hasNext();) {
        const auto hit = m.next();
        out += sheet.mid(last, hit.capturedStart() - last);
        auto it = names().find(hit.captured(1));
        out += it != names().end() ? (c().*(it->second)).name() : hit.captured(0);
        last = hit.capturedEnd();
    }
    return out + sheet.mid(last);
}

void style(QWidget* w, const QString& sheet)
{
    w->setProperty(kSheetProperty, sheet);
    w->setStyleSheet(css(sheet));
}

void apply(QApplication& app, const QString& id)
{
    QSettings settings;
    if (!id.isEmpty())
        settings.setValue(QStringLiteral("ui/theme"), id);
    const QString want = id.isEmpty() ? settings.value(QStringLiteral("ui/theme"), QStringLiteral("classic")).toString() : id;
    g_current = 0;
    for (size_t i = 0; i < kThemes.size(); ++i)
        if (want == QLatin1String(kThemes[i].id))
            g_current = i;
    const Colors& t = c();

    app.setStyle(new Style(QStyleFactory::create(QStringLiteral("Fusion"))));
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.panel);
    // Tooltips: black on pale yellow in every theme, the style Windows draws them in anyway.
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xe1));
    p.setColor(QPalette::ToolTipText, Qt::black);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.raised);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.bright);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.onAccent);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::PlaceholderText, t.faint);
    p.setColor(QPalette::Light, t.hover);
    p.setColor(QPalette::Midlight, t.raised);
    p.setColor(QPalette::Mid, t.line);
    p.setColor(QPalette::Dark, t.deep);
    p.setColor(QPalette::Shadow, t.deep);
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, role, t.faint);
    app.setPalette(p);
    app.setStyleSheet(css(QStringLiteral(
        "QToolTip { color: #000000; background-color: #ffffe1; border: 1px solid #767676; padding: 3px; }"
        "QSplitter::handle { background: @deep; }"
        "QToolBar { border: none; spacing: 2px; padding: 2px; }"
        "QToolButton { padding: 3px 6px; }"
        "QToolButton:checked { background: @checked; border-radius: 2px; }"
        "QToolBar#transport QToolButton { padding: 3px 5px; border-radius: 3px; }"
        "QToolBar#transport QToolButton:hover { background: @hover; }"
        "QTabWidget::pane { border: 1px solid @deep; }"
        "QTabBar::tab { background: @panel; color: @dim; padding: 4px 12px; border: 1px solid @deep; }"
        "QTabBar::tab:selected { background: @raised; color: @bright; border-bottom: 2px solid @accent; }")));
    // Widget style sheets made from the theme.
    for (QWidget* w : QApplication::allWidgets())
        if (const QVariant sheet = w->property(kSheetProperty); sheet.isValid())
            w->setStyleSheet(css(sheet.toString()));
    for (QWidget* w : QApplication::topLevelWidgets())
        for (QAction* a : w->findChildren<QAction*>())
            if (const QVariant icon = a->property(kIconProperty); icon.isValid())
                a->setIcon(transportIcon(Icon(icon.toInt())));
}

namespace {

void drawIcon(QPainter& p, Icon icon, const QColor& c, double s)
{
    // Drawn on a 24x24 grid, scaled by s.
    p.scale(s, s);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    auto tri = [&](double x0, double x1, double y0 = 5, double y1 = 19) { // pointing from x0 to x1
        QPainterPath path;
        path.moveTo(x0, y0);
        path.lineTo(x1, (y0 + y1) / 2);
        path.lineTo(x0, y1);
        path.closeSubpath();
        p.drawPath(path);
    };
    auto bar = [&](double x, double w = 2.6) { p.drawRect(QRectF(x, 5, w, 14)); };
    switch (icon) {
    case Icon::Play: tri(7, 19); break;
    case Icon::PlayFromStart: bar(4); tri(9, 20); break;
    case Icon::Pause: bar(7, 3.5); bar(13.5, 3.5); break;
    case Icon::Stop: p.drawRect(QRectF(6.5, 6.5, 11, 11)); break;
    case Icon::GoToStart: bar(3.5); tri(13, 6.5); tri(20.5, 14); break;
    case Icon::GoToEnd: tri(3.5, 10); tri(11, 17.5); bar(17.9); break;
    case Icon::PrevFrame: bar(6); tri(18, 9.5); break;
    case Icon::NextFrame: tri(6, 14.5); bar(15.4); break;
    case Icon::Loop: {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(4.5, 5.5, 15, 13), 30 * 16, 300 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPainterPath head;
        head.moveTo(15.5, 3.5);
        head.lineTo(21, 7.5);
        head.lineTo(15, 10.5);
        head.closeSubpath();
        p.drawPath(head);
        break;
    }
    }
}

QPixmap iconPixmap(Icon icon, const QColor& c)
{
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    drawIcon(p, icon, c, 2.0);
    return pm;
}

} // namespace

QIcon transportIcon(Icon icon)
{
    const Colors& t = c();
    QIcon ic;
    ic.addPixmap(iconPixmap(icon, icon == Icon::Play ? t.good : t.text), QIcon::Normal, QIcon::Off);
    ic.addPixmap(iconPixmap(icon, t.bright), QIcon::Active, QIcon::Off);
    ic.addPixmap(iconPixmap(icon, t.faint), QIcon::Disabled, QIcon::Off);
    ic.addPixmap(iconPixmap(icon, t.key), QIcon::Normal, QIcon::On); // checked (loop)
    ic.addPixmap(iconPixmap(icon, t.dark ? t.key.lighter(120) : t.key.darker(120)), QIcon::Active, QIcon::On);
    return ic;
}

void setIcon(QAction* a, Icon icon)
{
    a->setProperty(kIconProperty, int(icon));
    a->setIcon(transportIcon(icon));
}

} // namespace theme
