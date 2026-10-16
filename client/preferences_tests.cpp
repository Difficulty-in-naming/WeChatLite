#include "media_storage.h"
#include "preferences_dialog.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <filesystem>
#include <memory>

namespace
{
std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

bool directoryLink(const QString &target, const QString &link)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(nativePath(target), nativePath(link), error);
    return !error;
}

bool store(const QString &path, const QByteArray &contents, const QDateTime &modified = {})
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return false;
    return !modified.isValid() || file.setFileTime(modified, QFileDevice::FileModificationTime);
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class PreferencesTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir files;
    std::unique_ptr<QTemporaryDir> currentFiles, cacheFiles;
    QString cacheRoot;

    PreferencesDialog::Values defaults() const
    {
        PreferencesDialog::Values value;
        value.downloads = currentFiles->filePath("downloads");
        return value;
    }

  private slots:
    void initTestCase()
    {
        QVERIFY(files.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("WeChatLitePreferencesTests");
        QCoreApplication::setApplicationName("Preferences-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.filePath("settings"));
        auto font = qApp->font();
        font.setPixelSize(14);
        qApp->setFont(font);
        cacheRoot = QDir::cleanPath(MediaStorage::cacheDirectory());
        QVERIFY(QStandardPaths::isTestModeEnabled());
        QVERIFY(QDir::isAbsolutePath(cacheRoot));
        QVERIFY(cacheRoot.contains(QCoreApplication::applicationName()));
        QVERIFY(QDir().mkpath(cacheRoot));
    }

    void init()
    {
        currentFiles = std::make_unique<QTemporaryDir>(files.filePath("case-XXXXXX"));
        // The fixed cache API has no injected path. Its unique test-only namespace
        // contains only this QTemporaryDir; no existing application cache is opened.
        cacheFiles = std::make_unique<QTemporaryDir>(QDir(cacheRoot).filePath("fixture-XXXXXX"));
        QVERIFY(currentFiles->isValid() && cacheFiles->isValid());
        QSettings().clear();
        Theme::initialize("light");
        QCoreApplication::processEvents();
    }

    void dialogNavigatesAndAppliesOneSnapshotWithoutPersistingItself()
    {
        auto initial = defaults();
        QSettings().setValue("themeMode", "light");
        PreferencesDialog dialog(initial);
        QSignalSpy applied(&dialog, &PreferencesDialog::applied);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QSignalSpy usage(&dialog, &PreferencesDialog::usageRequested);
        QSignalSpy soundPreview(&dialog, &PreferencesDialog::previewSoundRequested);
        auto *navigation = dialog.findChild<QListWidget *>("preferencesNavigation");
        auto *pages = dialog.findChild<QStackedWidget *>("preferencesPages");
        auto *theme = dialog.findChild<QComboBox *>("preferencesTheme");
        auto *quota = dialog.findChild<QComboBox *>("preferencesCacheLimit");
        auto *sound = dialog.findChild<QCheckBox *>("preferencesSound");
        QVERIFY(navigation && pages && theme && quota && sound);
        dialog.show();
        QTRY_COMPARE(usage.size(), 1);
        QCOMPARE(usage.first()[0].toString(), initial.downloads);
        QCOMPARE(navigation->count(), 3);
        QCOMPARE(pages->currentWidget()->objectName(), QString("preferencesStoragePage"));
        for (int row = 0; row < navigation->count(); ++row)
        {
            const auto rect = navigation->visualItemRect(navigation->item(row));
            QVERIFY(rect.isValid());
            QCOMPARE(navigation->indexAt(rect.center()).row(), row);
            if (row)
                QVERIFY(navigation->visualItemRect(navigation->item(row - 1)).bottom() < rect.top());
        }
        QTest::mouseClick(navigation->viewport(), Qt::LeftButton, {}, navigation->visualItemRect(navigation->item(1)).center());
        QCOMPARE(pages->currentWidget()->objectName(), QString("preferencesAppearancePage"));
        theme->setCurrentIndex(theme->findData("dark"));
        QTest::mouseClick(navigation->viewport(), Qt::LeftButton, {}, navigation->visualItemRect(navigation->item(2)).center());
        QCOMPARE(pages->currentWidget()->objectName(), QString("preferencesNotificationPage"));
        QTest::mouseClick(sound, Qt::LeftButton);
        QTest::mouseClick(dialog.findChild<QPushButton *>("preferencesSoundPreview"), Qt::LeftButton);
        QCOMPARE(soundPreview.size(), 1); // A spy observes the request; no sound backend is connected.
        QTest::mouseClick(navigation->viewport(), Qt::LeftButton, {}, navigation->visualItemRect(navigation->item(0)).center());
        quota->setCurrentIndex(quota->findData(1024LL * 1024 * 1024));
        PreferencesDialog::Values saved;
        connect(&dialog, &PreferencesDialog::applied, &dialog, [&] { saved = dialog.values(); });
        QTest::mouseClick(dialog.findChild<QPushButton *>("preferencesSave"), Qt::LeftButton);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(accepted.size(), 1);
        QCOMPARE(saved.downloads, initial.downloads);
        QCOMPARE(saved.theme, QString("dark"));
        QVERIFY(!saved.notificationSound);
        QCOMPARE(saved.cacheLimitBytes, 1024LL * 1024 * 1024);
        QCOMPARE(initial.theme, QString("system"));
        QVERIFY(initial.notificationSound);
        QCOMPARE(QSettings().value("themeMode").toString(), QString("light"));
        QCOMPARE(Theme::mode(), QString("light")); // Applying and saving belong to the caller.
    }

    void cancelPreservesOriginalValuesAndNeverRequestsSoundAutomatically()
    {
        auto initial = defaults();
        initial.theme = "dark";
        initial.notificationSound = false;
        initial.cacheLimitBytes = 256LL * 1024 * 1024;
        PreferencesDialog dialog(initial);
        QSignalSpy applied(&dialog, &PreferencesDialog::applied);
        QSignalSpy soundPreview(&dialog, &PreferencesDialog::previewSoundRequested);
        dialog.findChild<QComboBox *>("preferencesTheme")->setCurrentIndex(1);
        dialog.findChild<QCheckBox *>("preferencesSound")->setChecked(true);
        dialog.show();
        QTest::mouseClick(dialog.findChild<QPushButton *>("preferencesCancel"), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(applied.isEmpty() && soundPreview.isEmpty());
        QVERIFY(!QSettings().contains("themeMode"));
        PreferencesDialog restored(initial);
        QCOMPARE(restored.values().theme, QString("dark"));
        QVERIFY(!restored.values().notificationSound);
        QCOMPARE(restored.values().cacheLimitBytes, 256LL * 1024 * 1024);
    }

    void staleUsageAndCleanupCallbacksCannotReplaceCurrentStatistics()
    {
        PreferencesDialog dialog(defaults());
        QSignalSpy usage(&dialog, &PreferencesDialog::usageRequested);
        QSignalSpy cleared(&dialog, &PreferencesDialog::clearCacheRequested);
        dialog.show();
        auto *downloads = dialog.findChild<QLabel *>("preferencesUsageDownloads");
        auto *cache = dialog.findChild<QLabel *>("preferencesUsageCache");
        auto *status = dialog.findChild<QLabel *>("preferencesUsageStatus");
        auto *refresh = dialog.findChild<QPushButton *>("preferencesRefreshUsage");
        auto *clear = dialog.findChild<QPushButton *>("preferencesClearCache");
        auto *choose = dialog.findChild<QPushButton *>("preferencesChooseDownloads");
        QTRY_COMPARE(usage.size(), 1);
        const auto first = usage.first()[1].toULongLong();
        dialog.setUsage(first, 1024, 2048);
        QVERIFY(!downloads->text().contains("正在统计"));
        QTest::mouseClick(refresh, Qt::LeftButton);
        QCOMPARE(usage.size(), 2);
        const auto second = usage.last()[1].toULongLong();
        QVERIFY(second > first);
        const auto waiting = downloads->text();
        dialog.setUsage(first, 9000000, 9000000, "obsolete scan error");
        QCOMPARE(downloads->text(), waiting);
        QVERIFY(!status->text().contains("obsolete"));
        QVERIFY(!refresh->isEnabled());
        dialog.setUsage(second, -1, 1024, "read denied");
        QVERIFY(downloads->text().contains("暂不可读"));
        QVERIFY(!cache->text().contains("暂不可读"));
        QVERIFY(status->text().contains("read denied"));
        QVERIFY(refresh->isEnabled());
        QTest::mouseClick(clear, Qt::LeftButton);
        QCOMPARE(cleared.size(), 1);
        QCOMPARE(cleared.first()[0].toString(), defaults().downloads);
        const auto clearId = cleared.first()[1].toULongLong();
        QVERIFY(!choose->isEnabled() && !clear->isEnabled() && !refresh->isEnabled());
        dialog.setUsage(second, 500, 500);
        dialog.cacheCleared(clearId + 1);
        QCOMPARE(usage.size(), 2);
        QVERIFY(!clear->isEnabled());
        dialog.cacheCleared(clearId, "protected file is in use");
        QCOMPARE(usage.size(), 3);
        const auto latest = usage.last()[1].toULongLong();
        QVERIFY(latest > clearId);
        const auto rescanning = cache->text();
        dialog.setUsage(second, 999, 999);
        QCOMPARE(cache->text(), rescanning);
        dialog.setUsage(latest, 1024, 512);
        QVERIFY(clear->isEnabled() && refresh->isEnabled() && choose->isEnabled());
        QVERIFY(status->text().contains("protected file is in use"));
    }

    void downloadCacheRootAndDescendantsAreRejectedButSiblingIsAllowed()
    {
        const QStringList unsafe{cacheRoot, QDir(cacheRoot).filePath("downloads"),
                                 QDir(cacheRoot).filePath("nested/../downloads")};
        for (const auto &path : unsafe)
        {
            auto value = defaults();
            value.downloads = path;
            PreferencesDialog dialog(value);
            QSignalSpy applied(&dialog, &PreferencesDialog::applied);
            dialog.show();
            QTest::mouseClick(dialog.findChild<QPushButton *>("preferencesSave"), Qt::LeftButton);
            QVERIFY(applied.isEmpty());
            QVERIFY(dialog.isVisible());
            auto *error = dialog.findChild<QLabel *>("preferencesValidation");
            QVERIFY(error->isVisible() && error->text().contains("缓存目录"));
        }
        auto sibling = defaults();
        sibling.downloads = cacheRoot + "-saved-files";
        PreferencesDialog allowed(sibling);
        QSignalSpy applied(&allowed, &PreferencesDialog::applied);
        allowed.show();
        QTest::mouseClick(allowed.findChild<QPushButton *>("preferencesSave"), Qt::LeftButton);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(allowed.result(), int(QDialog::Accepted));
    }

    void canonicalAliasIntoCacheCannotBeSelectedForDownloads()
    {
        const auto alias = currentFiles->filePath("cache-alias");
        if (!directoryLink(cacheFiles->path(), alias))
            QSKIP("This platform/account cannot create a directory symlink.");
        const auto unlink = qScopeGuard([&]
        {
            std::error_code error;
            std::filesystem::remove(nativePath(alias), error);
        });
        auto value = defaults();
        value.downloads = alias;
        PreferencesDialog dialog(value);
        QSignalSpy applied(&dialog, &PreferencesDialog::applied);
        dialog.show();
        QTest::mouseClick(dialog.findChild<QPushButton *>("preferencesSave"), Qt::LeftButton);
        QVERIFY(applied.isEmpty());
        QVERIFY(dialog.findChild<QLabel *>("preferencesValidation")->isVisible());
    }

    void themesPersistAndRestoreLightDarkAndSystemWithoutChangingTheSystem()
    {
        Theme::apply("dark");
        QCOMPARE(Theme::mode(), QString("dark"));
        QVERIFY(Theme::isDark());
        QCOMPARE(QSettings().value("themeMode").toString(), QString("dark"));
        const auto saved = QSettings().value("themeMode").toString();
        Theme::initialize("light");
        QVERIFY(!Theme::isDark());
        QCOMPARE(QSettings().value("themeMode").toString(), saved);
        Theme::initialize(saved);
        QVERIFY(Theme::isDark());
        Theme::apply("light");
        QVERIFY(!Theme::isDark());
        QCOMPARE(QSettings().value("themeMode").toString(), QString("light"));
        Theme::apply("system");
        QCoreApplication::processEvents();
        QCOMPARE(Theme::mode(), QString("system"));
        QCOMPARE(QSettings().value("themeMode").toString(), QString("system"));
        QCOMPARE(Theme::isDark(), QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        Theme::apply("dark");
        Theme::apply("light");
        Theme::apply("system");
        QCoreApplication::processEvents();
        QCOMPARE(Theme::mode(), QString("system"));
        QCOMPARE(Theme::isDark(), QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        auto value = defaults();
        value.theme = QSettings().value("themeMode").toString();
        PreferencesDialog restored(value);
        QCOMPARE(restored.values().theme, QString("system"));
    }

    void pruningKeepsInUsePartialAndUserFilesAndRemovesOldCacheFirst()
    {
        const auto oldest = cacheFiles->filePath("old-preview.gif");
        const auto newer = cacheFiles->filePath("new-preview.jpg");
        const auto active = cacheFiles->filePath("playing.wav");
        const auto partial = cacheFiles->filePath("download.part");
        const auto userFile = QDir(defaults().downloads).filePath("saved-user-file.bin");
        const auto baseTime = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
        QVERIFY(store(oldest, QByteArray(64, 'a'), baseTime));
        QVERIFY(store(newer, QByteArray(96, 'b'), baseTime.addSecs(1)));
        QVERIFY(store(active, QByteArray(128, 'c'), baseTime.addSecs(2)));
        QVERIFY(store(partial, QByteArray(256, 'd'), baseTime.addSecs(3)));
        QVERIFY(store(userFile, QByteArray(512, 'u')));
        const QSet<QString> protectedPaths{active, partial};
        const auto first = MediaStorage::prune(96 + 128 + 256, protectedPaths);
        QCOMPARE(first.removed, QStringList{oldest});
        QCOMPARE(first.retained, qint64(96 + 128 + 256));
        QVERIFY(!QFileInfo::exists(oldest));
        QCOMPARE(read(newer), QByteArray(96, 'b'));
        const auto cleared = MediaStorage::prune(0, protectedPaths);
        QCOMPARE(cleared.removed, QStringList{newer});
        QCOMPARE(cleared.retained, qint64(128 + 256));
        QCOMPARE(read(active), QByteArray(128, 'c'));
        QCOMPARE(read(partial), QByteArray(256, 'd'));
        QCOMPARE(read(userFile), QByteArray(512, 'u'));
        const auto usage = MediaStorage::measure(defaults().downloads);
        QCOMPARE(usage.downloads, qint64(512));
        QCOMPARE(usage.cache, qint64(128 + 256));
        const auto abandoned = cacheFiles->filePath(".abandoned.part");
        QVERIFY(store(abandoned, QByteArray(32, 'p'), baseTime));
        const auto reclaimed = MediaStorage::prune(0, protectedPaths);
        QCOMPARE(reclaimed.removed, QStringList{abandoned});
        QVERIFY(!QFileInfo::exists(abandoned));
    }

    void cachedFilesBecomeIndependentDatedDownloadsAndAreCountedSeparately()
    {
        const auto source = cacheFiles->filePath("preview.gif");
        const auto original = QByteArray("GIF89a-synthetic-cache-fixture");
        QVERIFY(store(source, original));
        const auto outside = currentFiles->filePath("not-in-downloads.bin");
        QVERIFY(store(outside, QByteArray(800, 'x')));
        QString error;
        const auto first = MediaStorage::saveCached(source, defaults().downloads, "../../saved.gif", error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(first).fileName(), QString("saved.gif"));
        QCOMPARE(QFileInfo(first).dir().dirName(), QDate::currentDate().toString("yyyy-MM-dd"));
        QCOMPARE(read(first), original);
        const auto second = MediaStorage::saveCached(source, defaults().downloads, "saved.gif", error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        QVERIFY(first != second);
        QCOMPARE(read(second), original);
        auto usage = MediaStorage::measure(defaults().downloads);
        QCOMPARE(usage.downloads, qint64(original.size() * 2));
        QCOMPARE(usage.cache, qint64(original.size()));
        MediaStorage::prune(0, {});
        QCOMPARE(read(first), original);
        QCOMPARE(read(second), original);
        QCOMPARE(read(outside), QByteArray(800, 'x'));
        usage = MediaStorage::measure(defaults().downloads);
        QCOMPARE(usage.downloads, qint64(original.size() * 2));
        QCOMPARE(usage.cache, qint64(0));
    }

    void measuringAndPruningNeverFollowDirectoryLinks()
    {
        const auto outside = currentFiles->filePath("outside");
        const auto outsideFile = QDir(outside).filePath("must-stay.bin");
        QVERIFY(store(outsideFile, QByteArray(4096, 'x')));
        const auto downloads = defaults().downloads;
        QVERIFY(store(QDir(downloads).filePath("own.bin"), QByteArray(7, 'd')));
        QVERIFY(store(cacheFiles->filePath("own.bin"), QByteArray(11, 'c')));
        const auto downloadLink = QDir(downloads).filePath("outside-link");
        const auto cacheLink = cacheFiles->filePath("outside-link");
        const auto unlink = qScopeGuard([&]
        {
            for (const auto &path : {downloadLink, cacheLink})
            {
                std::error_code error;
                std::filesystem::remove(nativePath(path), error);
            }
        });
        if (!directoryLink(outside, downloadLink) || !directoryLink(outside, cacheLink))
            QSKIP("This platform/account cannot create directory symlinks.");
        const auto usage = MediaStorage::measure(downloads);
        QCOMPARE(usage.downloads, qint64(7));
        QCOMPARE(usage.cache, qint64(11));
        QCOMPARE(MediaStorage::measure(downloadLink).downloads, qint64(0));
        MediaStorage::prune(0, {});
        QCOMPARE(read(outsideFile), QByteArray(4096, 'x'));
        QCOMPARE(read(QDir(downloads).filePath("own.bin")), QByteArray(7, 'd'));
    }

    void cleanup()
    {
        Theme::initialize("light");
        QCoreApplication::processEvents();
        cacheFiles.reset();
        currentFiles.reset();
        QSettings().clear();
    }

    void cleanupTestCase()
    {
        // Only remove the now-empty fixed test cache directory, never an ancestor
        // or any recursively enumerated real user location.
        QVERIFY(cacheRoot.contains(QCoreApplication::applicationName()));
        QVERIFY(QDir().rmdir(cacheRoot));
    }
};

QTEST_MAIN(PreferencesTests)
#include "preferences_tests.moc"
