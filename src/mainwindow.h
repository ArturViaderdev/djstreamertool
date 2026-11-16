#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QMainWindow>
#include <QStringList>
#include <atomic>
#include <mutex>
#include <vector>

#include "audio.h"
#include "camera.h"
#include "audiorecorder.h"
#include "streamer.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class PreviewWidget;
class LevelMeter;

class MainWindow : public QMainWindow {
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    QWidget* buildDestTab();
    QWidget* buildVideoTab();
    QWidget* buildAudioTab();
    void loadSettings();
    void saveSettings();

    void refreshCameras();
    void refreshModes();
    void restartCamera();
    void refreshAudioSources();
    void restartAudio();
    void updateAudioInfo();

    void onStartStop();
    void startLive();
    void stopLive(bool ask);
    void setLiveUi(bool live);

    void onPreviewTimer();
    void onMeterTimer();
    void onStatusTimer();
    void appendLog(const QString& line);
    QString elapsed() const;
    const AudioSourceInfo* currentSource() const;

    // interfaz
    PreviewWidget* preview_ = nullptr;
    LevelMeter* meter_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QCheckBox *twitchOn_ = nullptr, *youtubeOn_ = nullptr, *customOn_ = nullptr, *recordOn_ = nullptr,
              *recordMp3_ = nullptr;
    QLabel *statusLive_ = nullptr, *statusDest_ = nullptr, *statusRec_ = nullptr, *statusInfo_ = nullptr;
    QLabel* audioInfo_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget *tabDest_ = nullptr, *tabVideo_ = nullptr, *tabAudio_ = nullptr;
    QLineEdit *twitchKey_ = nullptr, *twitchServer_ = nullptr, *youtubeKey_ = nullptr, *youtubeServer_ = nullptr;
    QLineEdit* customUrl_ = nullptr;
    QSpinBox *twitchKbps_ = nullptr, *youtubeKbps_ = nullptr, *customKbps_ = nullptr;
    QComboBox *camDev_ = nullptr, *camMode_ = nullptr, *outRes_ = nullptr, *encoder_ = nullptr, *preset_ = nullptr;
    QComboBox *audioSrc_ = nullptr, *audioKbps_ = nullptr, *flacRate_ = nullptr, *flacBits_ = nullptr;
    QComboBox* mp3Quality_ = nullptr;
    QLineEdit* flacDir_ = nullptr;
    QSpinBox* avOffset_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    // motor
    Camera camera_;
    AudioCapture audio_;
    Streamer streamer_;
    AudioRecorder flac_, mp3_;
    std::vector<CamDevice> cams_;
    std::vector<AudioSourceInfo> sources_;
    std::atomic<bool> streamOn_{false}, recOn_{false};
    std::atomic<uint64_t> lastFrameUs_{0};

    std::mutex prevMtx_;
    QImage prevImg_;
    bool prevNew_ = false;

    int outW_ = 1280, outH_ = 720, camFps_ = 60;
    bool live_ = false, stopping_ = false, loading_ = false;
    QElapsedTimer liveTimer_, meterClock_;
    QStringList lastRecPaths_;
};
