#include "system_notifier.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

class NotifierTests final : public QObject
{
    Q_OBJECT

  private slots:
    void testModeNeverCreatesNativeResourcesOrActivation()
    {
        QVERIFY(QStandardPaths::isTestModeEnabled());
        SystemNotifier notifier;
        QSignalSpy requested(&notifier, &SystemNotifier::requested);
        QSignalSpy activated(&notifier, &SystemNotifier::activated);
        QVERIFY(!notifier.isActivationRelay());
        QVERIFY(notifier.children().isEmpty());
        notifier.showMessage("fixture@chatroom", QStringLiteral("测试群"), "fixture message");
        QTest::qWait(30);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(activated.count(), 0);
        QVERIFY(notifier.children().isEmpty());
    }

    void plainTextAndGroupIdentityRemainIntact()
    {
        SystemNotifier notifier;
        QSignalSpy requested(&notifier, &SystemNotifier::requested);
        const QString group = QStringLiteral("测试群<&\"@chatroom");
        const QString title = QStringLiteral("小明 <测试> & \"引用\"");
        const QString body = QStringLiteral("<b>文本</b> & 回复😀");
        notifier.showMessage(group, title, body);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first(), QVariantList({group, title, body}));
    }

    void invalidGroupCannotCreateNotificationIntent_data()
    {
        QTest::addColumn<QString>("group");
        QTest::newRow("empty") << QString();
        QTest::newRow("oversized") << QString(1025, 'x');
        QTest::newRow("newline") << QString("group\nother");
        QTest::newRow("control") << QString("group") + QChar(0x1f);
        QTest::newRow("null") << QString("group") + QChar(0);
    }

    void invalidGroupCannotCreateNotificationIntent()
    {
        QFETCH(QString, group);
        SystemNotifier notifier;
        QSignalSpy requested(&notifier, &SystemNotifier::requested);
        notifier.showMessage(group, "title", "body");
        QCOMPARE(requested.count(), 0);
        QVERIFY(notifier.children().isEmpty());
    }

    void groupLimitAndEmptyHeading()
    {
        SystemNotifier notifier;
        QSignalSpy requested(&notifier, &SystemNotifier::requested);
        const QString group(1024, 'g');
        notifier.showMessage(group, {}, {});
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first()[0].toString(), group);
        QCOMPARE(requested.first()[1].toString(), QStringLiteral("微信轻聊"));
        QCOMPARE(requested.first()[2].toString(), QString());
    }

    void boundedTextCannotSplitEmojiOrKeepControlCharacters()
    {
        SystemNotifier notifier;
        QSignalSpy requested(&notifier, &SystemNotifier::requested);
        notifier.showMessage("g", QString(127, 't') + QStringLiteral("😀tail"), QString(600, 'b'));
        QCOMPARE(requested.count(), 1);
        const auto heading = requested.first()[1].toString();
        const auto body = requested.first()[2].toString();
        QCOMPARE(heading, QString(127, 't') + QChar(0x2026));
        QCOMPARE(body, QString(512, 'b') + QChar(0x2026));
        QVERIFY(!heading.at(heading.size() - 2).isSurrogate());

        requested.clear();
        const QString controls = QString("first\nsecond\tthird") + QChar(0) + "last";
        notifier.showMessage("g", controls, controls);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first()[1].toString(), QString("first second thirdlast"));
        QCOMPARE(requested.first()[2].toString(), QString("first second thirdlast"));
    }

    void nativeActivationArgumentsAreRemovedWithoutDroppingClientArguments()
    {
        QProcess fixture;
        fixture.setProgram(QCoreApplication::applicationFilePath());
        fixture.setArguments({"--notifier-argument-fixture", "--notification-activation", "-Embedding",
                              "/eMbEdDiNg", "--server=https://example.invalid:5443", "--demo"});
        fixture.start();
        QVERIFY(fixture.waitForStarted(3000));
        QVERIFY(fixture.waitForFinished(5000));
        QCOMPARE(fixture.exitStatus(), QProcess::NormalExit);
        QCOMPARE(fixture.exitCode(), 0);
        QJsonParseError error;
        const auto result = QJsonDocument::fromJson(fixture.readAllStandardOutput(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(result.isArray());
        const auto arguments = result.array();
        QCOMPARE(arguments.size(), 4);
        QVERIFY(!arguments[0].toString().isEmpty());
        QCOMPARE(arguments[1].toString(), QString("--notifier-argument-fixture"));
        QCOMPARE(arguments[2].toString(), QString("--server=https://example.invalid:5443"));
        QCOMPARE(arguments[3].toString(), QString("--demo"));
    }
};

int main(int argc, char **argv)
{
    // Enable this before constructing any notifier: tests never register COM, request permission or notify.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication application(argc, argv);
    if (application.arguments().contains("--notifier-argument-fixture"))
    {
        QJsonArray arguments;
        for (const auto &argument : SystemNotifier::applicationArguments())
            arguments.append(argument);
        QFile output;
        if (!output.open(stdout, QIODevice::WriteOnly))
            return 1;
        output.write(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
        return output.flush() ? 0 : 1;
    }
    NotifierTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "notifier_tests.moc"
