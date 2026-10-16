#include "media_storage.h"
#include "preferences_dialog.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
#include <thread>

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
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.flush())
        return false;
    if (modified.isValid() && !file.setFileTime(modified, QFileDevice::FileModificationTime))
        return false;
    file.close();
    return !modified.isValid() || QFileInfo(path).lastModified() == modified;
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject media(const QString &id, const QString &kind, const QString &name, const QByteArray &contents)
{
    return {{"id", id}, {"kind", kind}, {"name", name}, {"size", contents.size()},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex())}};
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

    QString indexDirectory() const
    {
        return QDir(QFileInfo(currentFiles->path()).canonicalFilePath()).filePath("index");
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
        QCOMPARE(navigation->count(), 4);
        navigation->setCurrentRow(3);
        QCOMPARE(pages->currentWidget()->objectName(), QString("preferencesUpdatePage"));
        QVERIFY(dialog.findChild<QPushButton *>("updateCheck")->isEnabled());
        QVERIFY(dialog.findChild<QLabel *>("updateStatus")->text().contains("尚未检查"));
        navigation->setCurrentRow(0);
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
        QVERIFY(QFileInfo(oldest).lastModified() < QFileInfo(newer).lastModified());
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

    void chosenDownloadRootResolvesAliasButNestedLinksAreSkipped()
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
        // A user-selected root can be an alias (including macOS /var). Only
        // nested links are excluded; measuring the chosen root is read-only.
        const auto aliasUsage = MediaStorage::measure(downloadLink);
        QCOMPARE(aliasUsage.downloads, qint64(4096));
        QCOMPARE(aliasUsage.cache, qint64(11));
        MediaStorage::prune(0, {});
        QCOMPARE(read(outsideFile), QByteArray(4096, 'x'));
        QCOMPARE(read(QDir(downloads).filePath("own.bin")), QByteArray(7, 'd'));
        QVERIFY(!QFileInfo::exists(cacheFiles->filePath("own.bin")));
    }

    void videoPostersStaySeparateFromOriginalsAndUntrustedPaths()
    {
        auto files = std::make_shared<MediaStorage::LocalFiles>();
        const QJsonObject descriptor{{"id", "clip"}, {"kind", "video"}, {"size", 1024 * 1024},
                                     {"sha256", QString(64, 'a')}};
        const auto cover = cacheFiles->filePath("cover.jpg");
        QVERIFY(store(cover, QByteArray(100, 'j')));
        QJsonObject untrusted = descriptor;
        untrusted["_poster"] = currentFiles->filePath("private-photo.jpg");
        QJsonObject message{{"id", "m"}, {"group", "g"}, {"status", "received"}, {"media", QJsonArray{untrusted}}};
        QVERIFY(files->hydrate(message)["media"].toArray()[0].toObject()["_poster"].toString().isEmpty());
        files->rememberPoster("g", descriptor, cover);
        QCOMPARE(files->findPoster("g", descriptor), QFileInfo(cover).absoluteFilePath());
        QVERIFY(files->find("g", descriptor).isEmpty());
        QVERIFY(files->pathForId("g", "clip").isEmpty());
        QCOMPARE(files->hydrate(message)["media"].toArray()[0].toObject()["_poster"].toString(), QFileInfo(cover).absoluteFilePath());
        auto changed = descriptor; changed["sha256"] = QString(64, 'b');
        QVERIFY(files->findPoster("g", changed).isEmpty());
        QVERIFY(files->findPoster("other-group", descriptor).isEmpty());
        files->discard({cover});
        QVERIFY(files->findPoster("g", descriptor).isEmpty());
    }

    void scopedLocalFilesSurviveRestartWithoutTrustingPayloadPaths()
    {
        const auto index = indexDirectory();
        const auto contents = QByteArray("GIF89a-persistent-media-fixture");
        const auto original = cacheFiles->filePath("retained.gif");
        const QJsonObject descriptor{{"id", "retained"}, {"kind", "gif"}, {"name", "retained.gif"},
            {"size", contents.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex())}};
        QVERIFY(store(original, contents));
        {
            MediaStorage::LocalFiles first("server-account-A", index);
            first.remember("g", descriptor, original);
            QCOMPARE(first.find("g", descriptor), QFileInfo(original).absoluteFilePath());
        }
        MediaStorage::LocalFiles restarted("server-account-A", index);
        QCOMPARE(restarted.find("g", descriptor), QFileInfo(original).absoluteFilePath());
        auto payload = descriptor;
        payload["_local"] = currentFiles->filePath("untrusted-private.gif");
        payload["_poster"] = payload["_local"];
        const QJsonObject message{{"group", "g"}, {"media", QJsonArray{payload}}};
        const auto hydrated = restarted.hydrate(message)["media"].toArray()[0].toObject();
        QCOMPARE(hydrated["_local"].toString(), QFileInfo(original).absoluteFilePath());
        QVERIFY(hydrated["_poster"].toString().isEmpty());
        MediaStorage::LocalFiles anotherAccount("server-account-B", index);
        QVERIFY(anotherAccount.find("g", descriptor).isEmpty());
        QVERIFY(restarted.find("other-group", descriptor).isEmpty());
        for (const auto &field : {"id", "sha256", "size", "kind"})
        {
            auto changed = descriptor;
            changed[field] = QString::fromLatin1(field) == "size" ? QJsonValue(contents.size() + 1) :
                QJsonValue(QString::fromLatin1(field) == "kind" ? "image" : QString(64, 'b'));
            QVERIFY(restarted.find("g", changed).isEmpty());
        }
    }

    void originalAndPosterIndicesSurviveIndependentlyAndInvalidateChangedFiles()
    {
        const auto index = indexDirectory();
        const auto bytes = QByteArray(2048, 'v');
        const auto descriptor = media("clip", "video", "clip.mp4", bytes);
        const auto original = cacheFiles->filePath("clip.mp4");
        const auto poster = cacheFiles->filePath("poster.jpg");
        const auto modified = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
        QVERIFY(store(original, bytes, modified));
        QVERIFY(store(poster, QByteArray(100, 'p'), modified));
        {
            MediaStorage::LocalFiles first("scope", index);
            first.remember("g", descriptor, original);
            first.rememberPoster("g", descriptor, poster);
            // Original/poster keys use separate domains even if an allowed
            // synthetic group ID starts with the old "poster:" prefix.
            first.remember("poster:g", descriptor, original);
            QCOMPARE(first.findPoster("g", descriptor), QFileInfo(poster).absoluteFilePath());
            QCOMPARE(first.pathForId("poster:g", "clip"), QFileInfo(original).absoluteFilePath());
        }
        {
            MediaStorage::LocalFiles restarted("scope", index);
            QCOMPARE(restarted.find("g", descriptor), QFileInfo(original).absoluteFilePath());
            QCOMPARE(restarted.findPoster("g", descriptor), QFileInfo(poster).absoluteFilePath());
            QVERIFY(store(original, bytes, modified.addSecs(1))); // Even unchanged bytes with a new mtime expire.
            QVERIFY(restarted.find("g", descriptor).isEmpty());
            QCOMPARE(restarted.findPoster("g", descriptor), QFileInfo(poster).absoluteFilePath());
            QVERIFY(store(poster, QByteArray(101, 'p'), modified)); // Size changes also expire the cover alone.
            QVERIFY(restarted.findPoster("g", descriptor).isEmpty());
        }
        {
            MediaStorage::LocalFiles restarted("scope", index);
            QVERIFY(restarted.find("g", descriptor).isEmpty());
            QVERIFY(restarted.findPoster("g", descriptor).isEmpty());
            restarted.remember("g", descriptor, original);
            QVERIFY(QFile::remove(original));
            QVERIFY(restarted.find("g", descriptor).isEmpty());
        }
    }

    void legacyMediaRecoveryHashesDatedAndNumberedCandidatesOncePerBatch()
    {
        const auto index = indexDirectory();
        const auto downloads = defaults().downloads;
        const auto picture = QByteArray("GIF89a-legacy-picture");
        const auto video = QByteArray(128 * 1024, 'v');
        const auto gif = media("old-picture", "gif", "picture.gif", picture);
        const auto clip = media("old-video", "video", "clip.mp4", video);
        const auto wrong = QDir(downloads).filePath("2024-01-01/picture.gif");
        const auto good = QDir(downloads).filePath("2024-01-02/picture (2).gif");
        const auto movie = QDir(downloads).filePath("clip.mp4");
        QVERIFY(store(wrong, QByteArray(picture.size(), 'x'))); // Name/length are insufficient.
        QVERIFY(store(good, picture));
        QVERIFY(store(movie, video));
        MediaStorage::LocalFiles files("scope", index);
        const QJsonArray messages{
            QJsonObject{{"group", "g"}, {"media", QJsonArray{gif, clip}}},
            QJsonObject{{"group", "g"}, {"media", QJsonArray{gif}}}};
        QVERIFY(files.find("g", gif).isEmpty());
        std::thread worker([&] { files.recover(messages, downloads); });
        worker.join();
        QCOMPARE(files.find("g", gif), QFileInfo(good).canonicalFilePath());
        QCOMPARE(files.find("g", clip), QFileInfo(movie).canonicalFilePath());
        QVERIFY(files.findPoster("g", clip).isEmpty()); // Random legacy JPEGs cannot be assigned safely.
        MediaStorage::LocalFiles restarted("scope", index);
        QCOMPARE(restarted.find("g", gif), QFileInfo(good).canonicalFilePath());
        QCOMPARE(restarted.find("g", clip), QFileInfo(movie).canonicalFilePath());
        QCOMPARE(QDir(QFileInfo(good).absolutePath()).entryList(QDir::Files).size(), 1);
    }

    void legacyMediaRecoverySkipsPayloadPathsNestedFoldersAndCancelledHashes()
    {
        const auto downloads = defaults().downloads;
        const auto bytes = QByteArray(8 * 1024 * 1024, 'b');
        auto descriptor = media("large", "video", "large.mp4", bytes);
        const auto privateFile = currentFiles->filePath("private/large.mp4");
        const auto nested = QDir(downloads).filePath("2024-01-01/private/large.mp4");
        const auto invalidDate = QDir(downloads).filePath("2024-02-30/large.mp4");
        QVERIFY(store(privateFile, bytes));
        QVERIFY(store(nested, bytes));
        QVERIFY(store(invalidDate, bytes));
        descriptor["_local"] = privateFile;
        descriptor["_poster"] = privateFile;
        MediaStorage::LocalFiles files;
        std::thread miss([&] { files.recover("g", descriptor, downloads); });
        miss.join();
        QVERIFY(files.find("g", descriptor).isEmpty());
        const auto candidate = QDir(downloads).filePath("2024-01-01/large.mp4");
        QVERIFY(store(candidate, bytes));
        int polls = 0;
        std::thread cancelled([&] { files.recover("g", descriptor, downloads, [&] { return ++polls >= 32; }); });
        cancelled.join();
        QVERIFY(polls >= 32); // A large candidate is cancellable during its 64 KiB hash loop.
        QVERIFY(files.find("g", descriptor).isEmpty());
        QString recovered;
        std::thread retry([&] { recovered = files.recover("g", descriptor, downloads); });
        retry.join();
        QCOMPARE(recovered, QFileInfo(candidate).canonicalFilePath());
        const QJsonObject recalled{{"group", "other"}, {"status", "recalled"}, {"media", QJsonArray{descriptor}}};
        files.recover(QJsonArray{recalled}, downloads);
        QVERIFY(files.find("other", descriptor).isEmpty());
    }

    void persistentMediaIndexRejectsLinksCorruptionAndOversizedData()
    {
        // Canonical temporary roots avoid treating macOS's system /var alias as
        // a nested attacker-controlled link in this isolated fixture.
        const auto root = QFileInfo(currentFiles->path()).canonicalFilePath();
        const auto index = QDir(root).filePath("index");
        const auto folder = QDir(root).filePath("actual");
        const auto path = QDir(folder).filePath("file.bin");
        const auto contents = QByteArray("retained");
        const auto descriptor = media("id", "file", "file.bin", contents);
        QVERIFY(store(path, contents));
        {
            MediaStorage::LocalFiles files("scope", index);
            files.remember("g", descriptor, path);
        }
        const auto names = QDir(index).entryList({"*.json"}, QDir::Files);
        QCOMPARE(names.size(), 1);
        const auto indexFile = QDir(index).filePath(names.first());
        const auto validIndex = read(indexFile);
        QVERIFY(validIndex.size() < 4 * 1024 * 1024);
        QVERIFY(store(indexFile, QByteArray("{broken")));
        QVERIFY(MediaStorage::LocalFiles("scope", index).find("g", descriptor).isEmpty());
        QVERIFY(store(indexFile, QByteArray(4 * 1024 * 1024 + 1, 'x')));
        QVERIFY(MediaStorage::LocalFiles("scope", index).find("g", descriptor).isEmpty());
        auto changed = QJsonDocument::fromJson(validIndex).object();
        changed["scope"] = QString(64, '0');
        QVERIFY(store(indexFile, QJsonDocument(changed).toJson(QJsonDocument::Compact)));
        QVERIFY(MediaStorage::LocalFiles("scope", index).find("g", descriptor).isEmpty());
        QVERIFY(store(indexFile, validIndex));
        const auto moved = QDir(root).filePath("moved");
        QVERIFY(QDir().rename(folder, moved));
        const auto unlink = qScopeGuard([&]
        {
            std::error_code error;
            std::filesystem::remove(nativePath(folder), error);
        });
        if (!directoryLink(moved, folder)) QSKIP("This account cannot create a directory symlink.");
        QVERIFY(MediaStorage::LocalFiles("scope", index).find("g", descriptor).isEmpty());
        MediaStorage::LocalFiles files;
        files.remember("g", descriptor, path);
        QVERIFY(files.find("g", descriptor).isEmpty());
        files.recover("g", descriptor, root);
        QVERIFY(files.find("g", descriptor).isEmpty());
    }

    void sameScopeInstancesMergeDownloadsAndPersistExplicitDiscard()
    {
        const auto index = indexDirectory();
        const auto firstBytes = QByteArray("first"), secondBytes = QByteArray("second");
        const auto first = media("first", "file", "first.txt", firstBytes);
        const auto second = media("second", "file", "second.txt", secondBytes);
        const auto root = QFileInfo(currentFiles->path()).canonicalFilePath();
        const auto firstPath = QDir(root).filePath("first.txt"), secondPath = QDir(root).filePath("second.txt");
        QVERIFY(store(firstPath, firstBytes));
        QVERIFY(store(secondPath, secondBytes));
        MediaStorage::LocalFiles window("scope", index), history("scope", index);
        window.remember("g", first, firstPath);
        history.remember("g", second, secondPath);
        {
            MediaStorage::LocalFiles restarted("scope", index);
            QCOMPARE(restarted.find("g", first), QFileInfo(firstPath).absoluteFilePath());
            QCOMPARE(restarted.find("g", second), QFileInfo(secondPath).absoluteFilePath());
        }
        // The older window has not seen second, but its explicit discard must
        // also exclude the newer on-disk entry when it merges the snapshot.
        window.discard({secondPath});
        MediaStorage::LocalFiles restarted("scope", index);
        QCOMPARE(restarted.find("g", first), QFileInfo(firstPath).absoluteFilePath());
        QVERIFY(restarted.find("g", second).isEmpty());
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
