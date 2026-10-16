#include "voice_player.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#endif

namespace
{
void put16(QByteArray &bytes, qsizetype at, quint16 value) { qToLittleEndian(value, bytes.data() + at); }
void put32(QByteArray &bytes, qsizetype at, quint32 value) { qToLittleEndian(value, bytes.data() + at); }
QByteArray silence(int milliseconds, quint32 rate = 24000, quint16 channels = 1)
{
    const quint32 frames = quint32(quint64(rate) * milliseconds / 1000);
    const quint32 size = frames * channels * 2;
    QByteArray wav(44 + size, '\0');
    wav.replace(0, 4, "RIFF");
    put32(wav, 4, quint32(wav.size() - 8));
    wav.replace(8, 8, "WAVEfmt ");
    put32(wav, 16, 16);
    put16(wav, 20, 1);
    put16(wav, 22, channels);
    put32(wav, 24, rate);
    put32(wav, 28, rate * channels * 2);
    put16(wav, 32, channels * 2);
    put16(wav, 34, 16);
    wav.replace(36, 4, "data");
    put32(wav, 40, size);
    return wav;
}
} // namespace

class VoiceTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    int nextFile = 0;

    QString store(const QByteArray &bytes)
    {
        const auto path = directory.filePath(QString::number(++nextFile) + ".wav");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return {};
        return path;
    }

  private slots:
    void invalidFilesNeverStart_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto valid = silence(20);
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not-wav") << QByteArray("not a wave file");
        auto broken = valid;
        broken[0] = 'X';
        QTest::newRow("wrong-container") << broken;
        broken = valid;
        put32(broken, 4, 0xffffffff);
        QTest::newRow("riff-overflow") << broken;
        broken = valid;
        broken.append('x');
        QTest::newRow("trailing-bytes") << broken;
        broken = valid;
        put32(broken, 16, 0xffffffff);
        QTest::newRow("chunk-overflow") << broken;
        broken = valid;
        put16(broken, 20, 3);
        QTest::newRow("float-pcm") << broken;
        broken = valid;
        put16(broken, 34, 8);
        QTest::newRow("eight-bit") << broken;
        broken = valid;
        put16(broken, 22, 3);
        QTest::newRow("too-many-channels") << broken;
        broken = valid;
        put32(broken, 24, 7999);
        QTest::newRow("sample-rate-too-low") << broken;
        broken = valid;
        put32(broken, 24, 192001);
        QTest::newRow("sample-rate-too-high") << broken;
        broken = valid;
        put16(broken, 32, 4);
        QTest::newRow("incorrect-block-align") << broken;
        broken = valid;
        put32(broken, 28, 1);
        QTest::newRow("incorrect-byte-rate") << broken;
        broken = valid;
        broken.replace(12, 4, "JUNK");
        QTest::newRow("missing-format") << broken;
        broken = valid;
        broken.replace(36, 4, "JUNK");
        QTest::newRow("missing-data") << broken;
        broken = valid;
        put32(broken, 40, quint32(broken.size() - 45));
        QTest::newRow("incomplete-frame") << broken;
        broken = valid;
        broken.append(valid.mid(36));
        put32(broken, 4, quint32(broken.size() - 8));
        QTest::newRow("duplicate-data") << broken;
        broken = valid;
        broken.insert(36, valid.mid(12, 24));
        put32(broken, 4, quint32(broken.size() - 8));
        QTest::newRow("duplicate-format") << broken;
        broken = valid.left(36);
        broken.append("data", 4);
        put32(broken, 4, quint32(broken.size() - 8));
        QTest::newRow("truncated-chunk-header") << broken;
        broken = valid;
        broken.insert(12, QByteArray("JUNK\1\0\0\0x", 9));
        put32(broken, 4, quint32(broken.size() - 8));
        QTest::newRow("missing-chunk-padding") << broken;
        broken = valid;
        for (int i = 0; i < 1024; ++i)
            broken.append(QByteArray("JUNK\0\0\0\0", 8));
        put32(broken, 4, quint32(broken.size() - 8));
        QTest::newRow("too-many-chunks") << broken;
        QTest::newRow("too-long") << silence(600001, 8000);
    }
    void invalidFilesNeverStart()
    {
        QFETCH(QByteArray, bytes);
        VoicePlayer player;
        QSignalSpy state(&player, &VoicePlayer::playingChanged), errors(&player, &VoicePlayer::error);
        const auto path = store(bytes);
        QVERIFY(!path.isEmpty());
        QVERIFY(!player.play(path));
        QVERIFY(!player.isPlaying());
        QCOMPARE(state.count(), 0);
        QCOMPARE(errors.count(), 1);
        QVERIFY(!errors.first().first().toString().isEmpty());
    }
    void missingDirectoryAndOversizedFilesAreRejected()
    {
        VoicePlayer player;
        QSignalSpy errors(&player, &VoicePlayer::error);
        QVERIFY(!player.play(directory.filePath("missing.wav")));
        QVERIFY(!player.play(directory.path()));
        const auto path = store({});
        QFile sparse(path);
        QVERIFY(sparse.open(QIODevice::WriteOnly));
        QVERIFY(sparse.resize(16 * 1024 * 1024 + 1));
        sparse.close();
        QVERIFY(!player.play(path));
        QCOMPARE(errors.count(), 3);
        QVERIFY(!player.isPlaying());
    }
    void playbackFinishesOnceAndAcceptsPaddedMetadata()
    {
#ifdef Q_OS_WIN
        if (waveOutGetNumDevs() == 0)
            QSKIP("This Windows host has no playback device; WAV validation still runs.");
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Native playback is implemented for Windows and macOS.");
#endif
        auto wav = silence(120);
        wav.insert(12, QByteArray("JUNK\1\0\0\0x\0", 10));
        put32(wav, 4, quint32(wav.size() - 8));
        VoicePlayer player;
        QSignalSpy state(&player, &VoicePlayer::playingChanged), errors(&player, &VoicePlayer::error);
        const bool started = player.play(store(wav));
        QVERIFY2(started,
                 errors.isEmpty() ? "No playback state" : qPrintable(errors.last().first().toString()));
        QVERIFY(player.isPlaying());
        QCOMPARE(state.count(), 1);
        QCOMPARE(state.first().first().toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!player.isPlaying(), 4000);
        QCOMPARE(state.count(), 2);
        QCOMPARE(state.last().first().toBool(), false);
        QTest::qWait(100);
        QCOMPARE(state.count(), 2);
        QCOMPARE(errors.count(), 0);
    }
    void stoppingCannotRevivePlayback()
    {
#ifdef Q_OS_WIN
        if (waveOutGetNumDevs() == 0)
            QSKIP("This Windows host has no playback device; WAV validation still runs.");
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Native playback is implemented for Windows and macOS.");
#endif
        VoicePlayer player;
        QSignalSpy state(&player, &VoicePlayer::playingChanged), errors(&player, &VoicePlayer::error);
        const bool started = player.play(store(silence(400)));
        QVERIFY2(started,
                 errors.isEmpty() ? "No playback state" : qPrintable(errors.last().first().toString()));
        QTest::qWait(20);
        player.stop();
        QVERIFY(!player.isPlaying());
        QCOMPARE(state.count(), 2);
        player.stop();
        QTest::qWait(500);
        QVERIFY(!player.isPlaying());
        QCOMPARE(state.count(), 2);
        QCOMPARE(errors.count(), 0);
    }
    void switchingKeepsOnlyTheNewCompletion()
    {
#ifdef Q_OS_WIN
        if (waveOutGetNumDevs() == 0)
            QSKIP("This Windows host has no playback device; WAV validation still runs.");
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Native playback is implemented for Windows and macOS.");
#endif
        VoicePlayer player;
        QSignalSpy state(&player, &VoicePlayer::playingChanged), errors(&player, &VoicePlayer::error);
        const bool first = player.play(store(silence(250)));
        QVERIFY2(first,
                 errors.isEmpty() ? "No playback state" : qPrintable(errors.last().first().toString()));
        QTest::qWait(20);
        const bool second = player.play(store(silence(700, 24000, 2)));
        QVERIFY2(second,
                 errors.isEmpty() ? "No playback state" : qPrintable(errors.last().first().toString()));
        QCOMPARE(state.count(), 3);
        QCOMPARE(state[0].first().toBool(), true);
        QCOMPARE(state[1].first().toBool(), false);
        QCOMPARE(state[2].first().toBool(), true);
        QTest::qWait(320);
        QVERIFY(player.isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(!player.isPlaying(), 4000);
        QCOMPARE(state.count(), 4);
        QCOMPARE(errors.count(), 0);
    }
    void invalidReplacementStopsAndDestructorCancelsCallbacks()
    {
#ifdef Q_OS_WIN
        if (waveOutGetNumDevs() == 0)
            QSKIP("This Windows host has no playback device; WAV validation still runs.");
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QSKIP("Native playback is implemented for Windows and macOS.");
#endif
        VoicePlayer player;
        QSignalSpy state(&player, &VoicePlayer::playingChanged), errors(&player, &VoicePlayer::error);
        const bool started = player.play(store(silence(300)));
        QVERIFY2(started,
                 errors.isEmpty() ? "No playback state" : qPrintable(errors.last().first().toString()));
        QVERIFY(!player.play(store("bad WAV")));
        QVERIFY(!player.isPlaying());
        QCOMPARE(state.count(), 2);
        QCOMPARE(errors.count(), 1);
        QTest::qWait(350);
        QCOMPARE(state.count(), 2);
        QCOMPARE(errors.count(), 1);
        auto *temporary = new VoicePlayer;
        QSignalSpy temporaryErrors(temporary, &VoicePlayer::error);
        const bool restarted = temporary->play(store(silence(200)));
        QVERIFY2(restarted, temporaryErrors.isEmpty()
                                ? "No playback state"
                                : qPrintable(temporaryErrors.last().first().toString()));
        delete temporary;
        QTest::qWait(250); // Pending native notifications must not dereference a destroyed player.
    }
};

QTEST_GUILESS_MAIN(VoiceTests)
#include "voice_tests.moc"
