#pragma once
#include <QObject>
#include <QString>
#include <memory>

class QTimer;

// Local PCM WAV playback. Call on this object's thread, with a running Qt event loop.
class VoicePlayer final : public QObject
{
    Q_OBJECT
  public:
    explicit VoicePlayer(QObject *parent = nullptr);
    ~VoicePlayer() override;
    bool play(const QString &path);
    void stop();
    bool isPlaying() const;

  signals:
    void playingChanged(bool playing);
    void error(const QString &message);

  private:
    struct Native;
    std::unique_ptr<Native> native;
    QTimer *watchdog = nullptr;
    quint64 generation = 0;
    bool playing = false;
    void finish(quint64 expectedGeneration, const QString &message = {});
};
