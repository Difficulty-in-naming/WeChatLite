#include "voice_player.h"
#include <QByteArrayView>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <atomic>
#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QWinEventNotifier>
#include <Windows.h>
#include <mmsystem.h>
#elif defined(Q_OS_MACOS)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace
{
constexpr qint64 maximumFileBytes = 16 * 1024 * 1024;
constexpr qint64 maximumDurationMs = 10 * 60 * 1000;

struct Wav
{
    QByteArray bytes;
    qsizetype offset = 0;
    quint32 length = 0;
    quint32 sampleRate = 0;
    quint16 channels = 0;
    quint16 blockAlign = 0;
    int durationMs = 0;
};

bool readWav(const QString &path, Wav &wav, QString &message)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > maximumFileBytes)
    {
        message = "语音文件不存在或超过 16 MiB";
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        message = "无法读取语音文件";
        return false;
    }
    wav.bytes = file.read(maximumFileBytes + 1);
    if (file.error() != QFileDevice::NoError || wav.bytes.size() > maximumFileBytes)
    {
        message = "语音文件读取失败或超过 16 MiB";
        return false;
    }
    const QByteArrayView bytes(wav.bytes);
    const auto u16 = [&bytes](qsizetype at) { return qFromLittleEndian<quint16>(bytes.data() + at); };
    const auto u32 = [&bytes](qsizetype at) { return qFromLittleEndian<quint32>(bytes.data() + at); };
    const auto malformed = [&message]
    {
        message = "语音 WAV 文件结构损坏";
        return false;
    };
    if (bytes.size() < 44 || bytes.first(4) != QByteArrayView("RIFF", 4) ||
        bytes.sliced(8, 4) != QByteArrayView("WAVE", 4) || quint64(u32(4)) + 8 != quint64(bytes.size()))
        return malformed();
    bool formatSeen = false, dataSeen = false;
    int chunks = 0;
    for (qsizetype at = 12; at < bytes.size();)
    {
        if (++chunks > 1024 || bytes.size() - at < 8)
            return malformed();
        const auto id = bytes.sliced(at, 4);
        const quint64 length = u32(at + 4), start = quint64(at) + 8;
        const auto paddedLength = length + (length & 1);
        if (paddedLength > quint64(bytes.size()) - start)
            return malformed();
        if (id == QByteArrayView("fmt ", 4))
        {
            if (formatSeen || (length != 16 && length != 18))
                return malformed();
            formatSeen = true;
            if (u16(start) != 1 || u16(start + 14) != 16 || (length == 18 && u16(start + 16) != 0))
            {
                message = "语音只支持 16 位 PCM WAV";
                return false;
            }
            wav.channels = u16(start + 2);
            wav.sampleRate = u32(start + 4);
            wav.blockAlign = u16(start + 12);
            if ((wav.channels != 1 && wav.channels != 2) || wav.sampleRate < 8000 ||
                wav.sampleRate > 192000 || wav.blockAlign != wav.channels * 2 ||
                u32(start + 8) != wav.sampleRate * wav.blockAlign)
            {
                message = "语音采样率、声道或 PCM 数据格式无效";
                return false;
            }
        }
        else if (id == QByteArrayView("data", 4))
        {
            if (dataSeen || length == 0)
                return malformed();
            dataSeen = true;
            wav.offset = qsizetype(start);
            wav.length = quint32(length);
        }
        at = qsizetype(start + paddedLength);
    }
    if (!formatSeen || !dataSeen || wav.length % wav.blockAlign != 0)
        return malformed();
    const quint64 frames = wav.length / wav.blockAlign;
    const quint64 duration = (frames * 1000 + wav.sampleRate - 1) / wav.sampleRate;
    if (duration > maximumDurationMs)
    {
        message = "语音时长超过 10 分钟";
        return false;
    }
    wav.durationMs = int(duration);
    return true;
}
} // namespace

struct VoicePlayer::Native
{
    VoicePlayer *owner;
    quint64 generation;
    Wav wav;

    Native(VoicePlayer *player, quint64 value, Wav data)
        : owner(player), generation(value), wav(std::move(data))
    {
    }

#ifdef Q_OS_WIN
    HWAVEOUT output = nullptr;
    HANDLE completed = nullptr;
    WAVEHDR header{};
    QWinEventNotifier *notifier = nullptr;
    bool prepared = false;

    static QString failure(MMRESULT code)
    {
        wchar_t detail[MAXERRORLENGTH]{};
        waveOutGetErrorTextW(code, detail, MAXERRORLENGTH);
        return QString("语音播放失败（WinMM %1）：%2").arg(code).arg(QString::fromWCharArray(detail));
    }

    bool start(QString &message)
    {
        completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completed)
        {
            message = "无法创建语音播放完成事件";
            return false;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = wav.channels;
        format.nSamplesPerSec = wav.sampleRate;
        format.nBlockAlign = wav.blockAlign;
        format.nAvgBytesPerSec = wav.sampleRate * wav.blockAlign;
        format.wBitsPerSample = 16;
        auto result = waveOutOpen(&output, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(completed), 0,
                                  CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR)
        {
            message = failure(result);
            return false;
        }
        header.lpData = wav.bytes.data() + wav.offset;
        header.dwBufferLength = wav.length;
        result = waveOutPrepareHeader(output, &header, sizeof(header));
        if (result != MMSYSERR_NOERROR)
        {
            message = failure(result);
            return false;
        }
        prepared = true;
        // CALLBACK_EVENT also signals open/close. Only WHDR_DONE confirms our buffer completed.
        ResetEvent(completed);
        result = waveOutWrite(output, &header, sizeof(header));
        if (result != MMSYSERR_NOERROR)
        {
            message = failure(result);
            return false;
        }
        notifier = new QWinEventNotifier(completed, owner);
        QObject::connect(notifier, &QWinEventNotifier::activated, owner,
                         [player = owner, expected = generation]
                         {
                             if (player->generation != expected || !player->native)
                                 return;
                             auto *current = player->native.get();
                             ResetEvent(current->completed);
                             if (current->header.dwFlags & WHDR_DONE)
                                 player->finish(expected);
                         });
        return true;
    }

    ~Native()
    {
        if (notifier)
        {
            notifier->setEnabled(false);
            QObject::disconnect(notifier, nullptr, owner, nullptr);
            // Completion may be called from this notifier's event dispatch.
            notifier->deleteLater();
        }
        if (output)
        {
            waveOutReset(output);
            if (prepared)
                waveOutUnprepareHeader(output, &header, sizeof(header));
            waveOutClose(output);
        }
        if (completed)
            CloseHandle(completed);
    }
#elif defined(Q_OS_MACOS)
    AudioQueueRef output = nullptr;
    std::atomic_bool cancelled{false};
    std::atomic_bool draining{false};
    bool listening = false;

    static QString failure(OSStatus code)
    {
        return QString("语音播放失败（AudioQueue %1）").arg(qint64(code));
    }

    void postFinish(const QString &message = {})
    {
        if (cancelled.load())
            return;
        QMetaObject::invokeMethod(
            owner, [player = owner, expected = generation, message] { player->finish(expected, message); },
            Qt::QueuedConnection);
    }

    static void consumed(void *context, AudioQueueRef queue, AudioQueueBufferRef)
    {
        auto *self = static_cast<Native *>(context);
        if (self->cancelled.load())
            return;
        self->draining.store(true);
        // The only buffer has been consumed. Drain device audio before reporting completion.
        const auto result = AudioQueueStop(queue, false);
        if (result != noErr)
            self->postFinish(failure(result));
    }

    static void runningChanged(void *context, AudioQueueRef queue, AudioQueuePropertyID)
    {
        auto *self = static_cast<Native *>(context);
        if (self->cancelled.load() || !self->draining.load())
            return;
        UInt32 running = 1, size = sizeof(running);
        const auto result = AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &running, &size);
        if (result != noErr)
            self->postFinish(failure(result));
        else if (running == 0)
            self->postFinish();
    }

    bool start(QString &message)
    {
        AudioStreamBasicDescription format{};
        format.mSampleRate = wav.sampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format.mBytesPerPacket = wav.blockAlign;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = wav.blockAlign;
        format.mChannelsPerFrame = wav.channels;
        format.mBitsPerChannel = 16;
        // Native callback threads also work with QCoreApplication; only completion is queued to Qt.
        auto result = AudioQueueNewOutput(&format, consumed, this, nullptr, nullptr, 0, &output);
        if (result != noErr)
        {
            message = failure(result);
            return false;
        }
        result = AudioQueueAddPropertyListener(output, kAudioQueueProperty_IsRunning, runningChanged, this);
        if (result != noErr)
        {
            message = failure(result);
            return false;
        }
        listening = true;
        AudioQueueBufferRef buffer = nullptr;
        result = AudioQueueAllocateBuffer(output, wav.length, &buffer);
        if (result == noErr)
        {
            std::memcpy(buffer->mAudioData, wav.bytes.constData() + wav.offset, wav.length);
            buffer->mAudioDataByteSize = wav.length;
            result = AudioQueueEnqueueBuffer(output, buffer, 0, nullptr);
        }
        if (result != noErr)
        {
            message = failure(result);
            return false;
        }
        wav.bytes.clear(); // AudioQueue now owns its copy; release the file while audio is playing.
        result = AudioQueueStart(output, nullptr);
        if (result != noErr)
        {
            message = failure(result);
            return false;
        }
        return true;
    }

    ~Native()
    {
        cancelled.store(true);
        if (output)
        {
            if (listening)
                AudioQueueRemovePropertyListener(output, kAudioQueueProperty_IsRunning, runningChanged, this);
            AudioQueueStop(output, true);
            // Immediate, synchronous disposal keeps the callback context alive until callbacks stop.
            AudioQueueDispose(output, true);
        }
    }
#else
    bool start(QString &message)
    {
        message = "当前系统暂不支持语音播放";
        return false;
    }
#endif
};

VoicePlayer::VoicePlayer(QObject *parent) : QObject(parent), watchdog(new QTimer(this))
{
    watchdog->setSingleShot(true);
    QObject::connect(watchdog, &QTimer::timeout, this,
                     [this] { finish(generation, "语音播放超时，已停止"); });
}

VoicePlayer::~VoicePlayer()
{
    ++generation;
    watchdog->stop();
    native.reset();
}

bool VoicePlayer::play(const QString &path)
{
    Q_ASSERT(QThread::currentThread() == thread());
    stop();
    Wav wav;
    QString message;
    if (!readWav(path, wav, message))
    {
        emit error(message);
        return false;
    }
    const auto duration = wav.durationMs;
    auto next = std::make_unique<Native>(this, generation, std::move(wav));
    if (!next->start(message))
    {
        next.reset();
        emit error(message);
        return false;
    }
    native = std::move(next);
    playing = true;
    watchdog->start(duration + 5000);
    emit playingChanged(true);
    return true;
}

void VoicePlayer::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    ++generation;
    watchdog->stop();
    native.reset();
    if (playing)
    {
        playing = false;
        emit playingChanged(false);
    }
}

bool VoicePlayer::isPlaying() const { return playing; }

void VoicePlayer::finish(quint64 expectedGeneration, const QString &message)
{
    if (expectedGeneration != generation || !playing)
        return;
    stop();
    if (!message.isEmpty())
        emit error(message);
}
