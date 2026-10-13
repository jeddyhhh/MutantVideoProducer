#include "core/Brand.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MvpBrandTest"));
    QTemporaryDir fixture;
    if (!fixture.isValid()) return 1;
    QSettings previous(fixture.filePath("old.ini"), QSettings::IniFormat);
    QSettings current(fixture.filePath("new.ini"), QSettings::IniFormat);
    previous.setValue("profiles/list", QByteArray("profiles and media homes"));
    previous.setValue("profiles/active", "original-id");
    previous.setValue("mcp/port", 47811);
    previous.setValue("theme", "classic");
    current.setValue("theme", "neon");
    int failures = 0;
    auto check = [&](bool ok, const char* text) { qInfo().noquote() << (ok ? "PASS" : "FAIL") << text; if (!ok) ++failures; };
    check(brand::migratePreferences(previous,current), "migration succeeds");
    check(current.value("profiles/list") == previous.value("profiles/list") && current.value("profiles/active")==previous.value("profiles/active"), "profile IDs and homes retained");
    check(current.value("mcp/port").toInt()==47811, "MCP connection retained");
    check(current.value("theme").toString()=="neon", "new settings not overwritten");
    check(previous.value("profiles/active").toString()=="original-id" && !previous.contains("migration/legacyPreferences"), "old settings untouched");
    current.remove("profiles/active");
    check(brand::migratePreferences(previous,current) && !current.contains("profiles/active"), "one-time migration does not resurrect deleted settings");
    qputenv("MONTAGE_REBRAND_TEST", "legacy");
    qputenv("MONTAGE_REBRAND_PRIORITY", "legacy");
    qputenv("MVP_REBRAND_PRIORITY", "new");
    brand::initializeCompatibility();
    check(qgetenv("MVP_REBRAND_TEST")=="legacy" && qgetenv("MVP_REBRAND_PRIORITY")=="new", "legacy environment supported with new override priority");
    check(brand::storageLocation(QStandardPaths::AppDataLocation)==QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), "isolated test data is not redirected to user data");
    return failures;
}
