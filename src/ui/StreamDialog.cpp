#include "ui/StreamDialog.h"

#include "ui/Theme.h"

#include "ui/Help.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace {

struct Service {
    const char* name;
    const char* server; // the key goes after it
    const char* hint;
};
const Service kServices[] = {
    {"Twitch", "rtmp://live.twitch.tv/app/", "The key: Twitch dashboard > Settings > Stream > Primary Stream Key."},
    {"YouTube", "rtmp://a.rtmp.youtube.com/live2/", "The key: YouTube Studio > Create > Go Live > Stream key."},
    {"Facebook Live", "rtmps://live-api-s.facebook.com:443/rtmp/", "The key: Facebook Live Producer > Streaming software > Stream key."},
    {"Kick", "", "Paste the Stream URL and Stream Key from Kick's dashboard (Settings > Stream URL & Key)."},
    {"Custom (RTMP / RTMPS / SRT)", "", "Any server: rtmp://, rtmps://, srt:// or udp://. The key (if it has one) is added to the end."},
};
constexpr int kCustom = 4;

QByteArray protect(const QByteArray& plain)
{
#ifdef Q_OS_WIN
    if (plain.isEmpty())
        return {};
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))}, out{};
    if (!CryptProtectData(&in, L"Mutant Video Producer stream key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const QByteArray r(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return r;
#else
    return plain;
#endif
}

QByteArray unprotect(const QByteArray& sealed)
{
#ifdef Q_OS_WIN
    if (sealed.isEmpty())
        return {};
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData()))}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    const QByteArray r(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
#else
    return sealed;
#endif
}

QString storedKey()
{
    return QString::fromUtf8(unprotect(QSettings().value(QStringLiteral("stream/key")).toByteArray()));
}

} // namespace

StreamDialog::StreamDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Stream Settings"));
    setMinimumWidth(560);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    m_service = new QComboBox;
    m_service->setObjectName(QStringLiteral("streamService"));
    for (const Service& s : kServices)
        m_service->addItem(QString::fromLatin1(s.name));
    form->addRow(tr("Service"), m_service);
    m_server = new QLineEdit;
    m_server->setObjectName(QStringLiteral("streamServer"));
    m_server->setPlaceholderText(QStringLiteral("rtmp://..."));
    form->addRow(tr("Server"), m_server);
    m_key = new QLineEdit;
    m_key->setObjectName(QStringLiteral("streamKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(tr("paste your stream key"));
    m_key->setToolTip(tr("Kept encrypted for your Windows account. Anyone with the key can stream to your channel: keep it private."));
    form->addRow(tr("Stream key"), m_key);
    m_kbps = new QSpinBox;
    m_kbps->setObjectName(QStringLiteral("streamBitrate"));
    m_kbps->setRange(500, 50000);
    m_kbps->setSingleStep(500);
    m_kbps->setSuffix(tr(" kb/s"));
    m_kbps->setToolTip(tr("The picture's bitrate. Twitch: up to 6000 (720p60 4500, 1080p60 6000). YouTube: 1080p 4500-9000.\n"
                          "Keep it well under your upload speed."));
    form->addRow(tr("Bitrate"), m_kbps);
    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    theme::style(m_hint, QStringLiteral("color: @dim;"));
    layout->addWidget(m_hint);
    auto* note = new QLabel(tr("The stream is the Live output with its effects and overlays, at the Live tab's size and frame rate,\n"
                               "with the Audio input as its sound (silence if there is none)."));
    theme::style(note, QStringLiteral("color: @dim;"));
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("live.md#streaming-go-live"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QSettings s;
    const int service = std::clamp(s.value(QStringLiteral("stream/service"), 0).toInt(), 0, kCustom);
    m_service->setCurrentIndex(service);
    m_server->setText(s.value(QStringLiteral("stream/server"), QString::fromLatin1(kServices[service].server)).toString());
    m_key->setText(storedKey());
    m_kbps->setValue(s.value(QStringLiteral("stream/kbps"), 4500).toInt());
    m_hint->setText(tr(kServices[service].hint));
    connect(m_service, qOverload<int>(&QComboBox::currentIndexChanged), this, &StreamDialog::updateServer);
}

void StreamDialog::updateServer()
{
    const int i = m_service->currentIndex();
    if (i < 0 || i >= int(std::size(kServices)))
        return;
    const Service& s = kServices[i];
    if (*s.server)
        m_server->setText(QString::fromLatin1(s.server));
    else
        m_server->clear();
    m_hint->setText(tr(s.hint));
}

void StreamDialog::save()
{
    QSettings s;
    s.setValue(QStringLiteral("stream/service"), m_service->currentIndex());
    s.setValue(QStringLiteral("stream/server"), m_server->text().trimmed());
    s.setValue(QStringLiteral("stream/key"), protect(m_key->text().trimmed().toUtf8()));
    s.setValue(QStringLiteral("stream/kbps"), m_kbps->value());
}

QString StreamDialog::streamUrl()
{
    QSettings s;
    QString server = s.value(QStringLiteral("stream/server")).toString().trimmed();
    const QString key = storedKey();
    if (server.isEmpty())
        return {};
    const int service = s.value(QStringLiteral("stream/service"), 0).toInt();
    if (key.isEmpty())
        return service == kCustom ? server : QString(); // a known site needs its key
    if (!server.startsWith(QLatin1String("rtmp"))) // SRT / UDP carry their own settings in the address
        return server;
    if (!server.endsWith(QLatin1Char('/')))
        server += QLatin1Char('/');
    return server + key;
}

int StreamDialog::bitrateKbps()
{
    return QSettings().value(QStringLiteral("stream/kbps"), 4500).toInt();
}

QString StreamDialog::serviceName()
{
    const int service = std::clamp(QSettings().value(QStringLiteral("stream/service"), 0).toInt(), 0, kCustom);
    return service == kCustom ? tr("your server") : QString::fromLatin1(kServices[service].name);
}

