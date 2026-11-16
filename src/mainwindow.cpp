#include "mainwindow.h"

#include <sys/statvfs.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <thread>
#include <tuple>

#include <linux/videodev2.h>

#include "log.h"
#include "widgets.h"

static const char* kTwitchServer = "rtmp://ingest.global-contribute.live-video.net/app";
static const char* kYouTubeServer = "rtmp://a.rtmp.youtube.com/live2";

static QString fmtDuration(qint64 ms) {
    qint64 s = ms / 1000;
    return QString::asprintf("%02lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("DJ Live Streamer"));
    buildUi();
    logSetSink([this](const std::string& line) {
        QString l = QString::fromStdString(line);
        QMetaObject::invokeMethod(this, [this, l] { appendLog(l); }, Qt::QueuedConnection);
    });
    loadSettings();
    LOG("registro en %s", logPath().c_str());
    restartCamera();
    restartAudio();

    auto* tPrev = new QTimer(this);
    connect(tPrev, &QTimer::timeout, this, [this] { onPreviewTimer(); });
    tPrev->start(33);
    auto* tMeter = new QTimer(this);
    connect(tMeter, &QTimer::timeout, this, [this] { onMeterTimer(); });
    tMeter->start(33);
    meterClock_.start();
    auto* tStat = new QTimer(this);
    connect(tStat, &QTimer::timeout, this, [this] { onStatusTimer(); });
    tStat->start(1000);
    onStatusTimer();
}

MainWindow::~MainWindow() {
    logSetSink(nullptr);
    streamOn_ = false;
    recOn_ = false;
    camera_.stop();
    audio_.stop();
    streamer_.stop();
    flac_.stop();
    mp3_.stop();
}

// ---------------------------------------------------------------------------
// Interfaz
// ---------------------------------------------------------------------------
void MainWindow::buildUi() {
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);

    // Arriba: cámara + vúmetro
    auto* top = new QWidget;
    auto* topL = new QHBoxLayout(top);
    topL->setContentsMargins(0, 0, 0, 0);
    preview_ = new PreviewWidget;
    topL->addWidget(preview_, 1);
    auto* meterBox = new QVBoxLayout;
    auto* meterTitle = new QLabel(QStringLiteral("<b>Entrada</b>"));
    meterTitle->setAlignment(Qt::AlignCenter);
    meterBox->addWidget(meterTitle);
    meter_ = new LevelMeter;
    meterBox->addWidget(meter_, 1);
    topL->addLayout(meterBox);

    // Controles
    auto* ctl = new QHBoxLayout;
    startBtn_ = new QPushButton(QStringLiteral("▶  EMPEZAR"));
    startBtn_->setMinimumHeight(44);
    startBtn_->setMinimumWidth(190);
    QFont bf = startBtn_->font();
    bf.setBold(true);
    bf.setPointSizeF(bf.pointSizeF() * 1.25);
    startBtn_->setFont(bf);
    connect(startBtn_, &QPushButton::clicked, this, [this] { onStartStop(); });
    ctl->addWidget(startBtn_);
    twitchOn_ = new QCheckBox(QStringLiteral("Twitch"));
    youtubeOn_ = new QCheckBox(QStringLiteral("YouTube"));
    customOn_ = new QCheckBox(QStringLiteral("Personalizado"));
    recordOn_ = new QCheckBox(QStringLiteral("Grabar FLAC"));
    recordMp3_ = new QCheckBox(QStringLiteral("Grabar MP3"));
    for (auto* c : {twitchOn_, youtubeOn_, customOn_, recordOn_, recordMp3_}) {
        ctl->addSpacing(10);
        ctl->addWidget(c);
    }
    ctl->addStretch(1);

    statusLive_ = new QLabel;
    QFont lf = statusLive_->font();
    lf.setBold(true);
    lf.setPointSizeF(lf.pointSizeF() * 1.2);
    statusLive_->setFont(lf);
    statusDest_ = new QLabel;
    statusRec_ = new QLabel;
    statusInfo_ = new QLabel;
    for (auto* l : {statusDest_, statusRec_, statusInfo_}) {
        l->setTextFormat(Qt::RichText);
        l->setWordWrap(true);
    }

    tabs_ = new QTabWidget;
    tabDest_ = buildDestTab();
    tabVideo_ = buildVideoTab();
    tabAudio_ = buildAudioTab();
    tabs_->addTab(tabDest_, QStringLiteral("Destinos"));
    tabs_->addTab(tabVideo_, QStringLiteral("Cámara y vídeo"));
    tabs_->addTab(tabAudio_, QStringLiteral("Audio y grabación"));
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    tabs_->addTab(log_, QStringLiteral("Registro"));

    auto* bottom = new QWidget;
    auto* bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addLayout(ctl);
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(statusLive_);
    statusRow->addSpacing(16);
    statusRow->addWidget(statusDest_, 1);
    bl->addLayout(statusRow);
    bl->addWidget(statusRec_);
    bl->addWidget(statusInfo_);
    bl->addWidget(tabs_, 1);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(top);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    root->addWidget(split);
    setCentralWidget(central);
    resize(1180, 960);
}

QWidget* MainWindow::buildDestTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto mkKbps = [](int def, const QString& tip) {
        auto* s = new QSpinBox;
        s->setRange(500, 20000);
        s->setSingleStep(250);
        s->setSuffix(QStringLiteral(" kbps"));
        s->setValue(def);
        s->setToolTip(tip);
        return s;
    };

    auto* row = new QHBoxLayout;
    auto* gTw = new QGroupBox(QStringLiteral("Twitch"));
    auto* fTw = new QFormLayout(gTw);
    twitchKey_ = new QLineEdit;
    twitchKey_->setEchoMode(QLineEdit::Password);
    twitchKey_->setPlaceholderText(QStringLiteral("live_xxxxxxxx"));
    twitchServer_ = new QLineEdit;
    twitchServer_->setPlaceholderText(kTwitchServer);
    twitchKbps_ = mkKbps(4500, QStringLiteral("Twitch: máximo 6000 kbps. Para 720p60 recomienda 4500."));
    fTw->addRow(QStringLiteral("Clave:"), twitchKey_);
    fTw->addRow(QStringLiteral("Servidor:"), twitchServer_);
    fTw->addRow(QStringLiteral("Vídeo:"), twitchKbps_);
    row->addWidget(gTw);

    auto* gYt = new QGroupBox(QStringLiteral("YouTube"));
    auto* fYt = new QFormLayout(gYt);
    youtubeKey_ = new QLineEdit;
    youtubeKey_->setEchoMode(QLineEdit::Password);
    youtubeKey_->setPlaceholderText(QStringLiteral("xxxx-xxxx-xxxx-xxxx-xxxx"));
    youtubeServer_ = new QLineEdit;
    youtubeServer_->setPlaceholderText(kYouTubeServer);
    youtubeKbps_ = mkKbps(6000, QStringLiteral("YouTube admite más que Twitch; 6000 kbps va bien para 720p60."));
    fYt->addRow(QStringLiteral("Clave:"), youtubeKey_);
    fYt->addRow(QStringLiteral("Servidor:"), youtubeServer_);
    fYt->addRow(QStringLiteral("Vídeo:"), youtubeKbps_);
    row->addWidget(gYt);
    v->addLayout(row);

    auto* gCu = new QGroupBox(QStringLiteral("Personalizado / prueba"));
    auto* fCu = new QFormLayout(gCu);
    customUrl_ = new QLineEdit;
    customUrl_->setPlaceholderText(QStringLiteral("rtmp://servidor/app/clave   o   ~/Vídeos/prueba.flv para probar sin emitir"));
    customKbps_ = mkKbps(6000, QString());
    fCu->addRow(QStringLiteral("URL o archivo:"), customUrl_);
    fCu->addRow(QStringLiteral("Vídeo:"), customKbps_);
    v->addWidget(gCu);

    auto* show = new QCheckBox(QStringLiteral("Mostrar las claves"));
    connect(show, &QCheckBox::toggled, this, [this](bool on) {
        twitchKey_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        youtubeKey_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    v->addWidget(show);
    auto* note = new QLabel(QStringLiteral(
        "Si se corta la conexión con un destino se reintenta cada 5 s sin parar el otro ni la grabación. "
        "Las claves se guardan en ~/.config/djlivestreamer/djlivestreamer.conf (sólo legible por tu usuario)."));
    note->setWordWrap(true);
    v->addWidget(note);
    v->addStretch(1);
    return w;
}

QWidget* MainWindow::buildVideoTab() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    camDev_ = new QComboBox;
    auto* refresh = new QPushButton(QStringLiteral("↻"));
    refresh->setToolTip(QStringLiteral("Volver a buscar cámaras"));
    refresh->setMaximumWidth(40);
    connect(refresh, &QPushButton::clicked, this, [this] {
        refreshCameras();
        restartCamera();
    });
    auto* devRow = new QHBoxLayout;
    devRow->addWidget(camDev_, 1);
    devRow->addWidget(refresh);
    f->addRow(QStringLiteral("Cámara:"), devRow);
    camMode_ = new QComboBox;
    camMode_->setToolTip(QStringLiteral("Modos que ofrece la cámara. A 60 fps casi todas las webcams usan MJPEG."));
    f->addRow(QStringLiteral("Modo de captura:"), camMode_);
    outRes_ = new QComboBox;
    outRes_->addItem(QStringLiteral("Igual que la cámara"), QStringLiteral("0x0"));
    outRes_->addItem(QStringLiteral("1280×720 (720p)"), QStringLiteral("1280x720"));
    outRes_->addItem(QStringLiteral("1920×1080 (1080p)"), QStringLiteral("1920x1080"));
    outRes_->addItem(QStringLiteral("960×540"), QStringLiteral("960x540"));
    outRes_->addItem(QStringLiteral("854×480"), QStringLiteral("854x480"));
    f->addRow(QStringLiteral("Resolución del directo:"), outRes_);
    encoder_ = new QComboBox;
    encoder_->addItem(QStringLiteral("Automático (NVENC si hay GPU NVIDIA, si no x264)"), QStringLiteral("auto"));
    encoder_->addItem(QStringLiteral("NVENC (GPU NVIDIA)"), QStringLiteral("nvenc"));
    encoder_->addItem(QStringLiteral("x264 (CPU)"), QStringLiteral("x264"));
    f->addRow(QStringLiteral("Codificador:"), encoder_);
    preset_ = new QComboBox;
    for (int i = 1; i <= 7; i++)
        preset_->addItem(QStringLiteral("p%1%2").arg(i).arg(i == 1 ? " (más rápido)" : i == 7 ? " (mejor calidad)" : ""),
                         QStringLiteral("p%1").arg(i));
    preset_->setCurrentIndex(4);
    f->addRow(QStringLiteral("Preset NVENC:"), preset_);
    auto* note = new QLabel(QStringLiteral(
        "La imagen se emite tal cual la ve la cámara (sin espejo). Si la proporción de la cámara no es 16:9 se recorta "
        "al centro. Con poca luz muchas webcams bajan de 60 a 30 fps: el programa desactiva la «prioridad de "
        "exposición» para mantener los fps; si aun así baja, hace falta más luz."));
    note->setWordWrap(true);
    f->addRow(note);

    connect(camDev_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (loading_) return;
        refreshModes();
        restartCamera();
    });
    connect(camMode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!loading_) restartCamera();
    });
    connect(outRes_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!loading_) restartCamera();
    });
    return w;
}

QWidget* MainWindow::buildAudioTab() {
    auto* w = new QWidget;
    auto* f = new QFormLayout(w);
    audioSrc_ = new QComboBox;
    auto* refresh = new QPushButton(QStringLiteral("↻"));
    refresh->setToolTip(QStringLiteral("Volver a buscar entradas de audio"));
    refresh->setMaximumWidth(40);
    connect(refresh, &QPushButton::clicked, this, [this] {
        refreshAudioSources();
        restartAudio();
    });
    auto* srcRow = new QHBoxLayout;
    srcRow->addWidget(audioSrc_, 1);
    srcRow->addWidget(refresh);
    f->addRow(QStringLiteral("Entrada de audio:"), srcRow);
    audioInfo_ = new QLabel;
    audioInfo_->setWordWrap(true);
    f->addRow(QString(), audioInfo_);
    audioKbps_ = new QComboBox;
    for (int k : {128, 160, 192, 256, 320}) audioKbps_->addItem(QStringLiteral("%1 kbps").arg(k), k);
    audioKbps_->setCurrentIndex(1);
    audioKbps_->setToolTip(QStringLiteral("Twitch admite como máximo 160 kbps; YouTube más. Se usa el mismo para los dos."));
    f->addRow(QStringLiteral("Audio del directo (AAC):"), audioKbps_);

    flacDir_ = new QLineEdit;
    auto* browse = new QPushButton(QStringLiteral("Elegir…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Carpeta para las grabaciones"), flacDir_->text());
        if (!d.isEmpty()) flacDir_->setText(d);
    });
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(flacDir_, 1);
    dirRow->addWidget(browse);
    f->addRow(QStringLiteral("Carpeta de grabación:"), dirRow);
    flacRate_ = new QComboBox;
    flacRate_->addItem(QStringLiteral("La de la entrada (sin remuestrear, recomendado)"), 0);
    flacRate_->addItem(QStringLiteral("44100 Hz (remuestrea si la entrada va a 48 kHz)"), 44100);
    flacRate_->addItem(QStringLiteral("48000 Hz (remuestrea si la entrada va a 44,1 kHz)"), 48000);
    f->addRow(QStringLiteral("Frecuencia de grabación:"), flacRate_);
    flacBits_ = new QComboBox;
    flacBits_->addItem(QStringLiteral("16 bits"), 16);
    flacBits_->addItem(QStringLiteral("24 bits"), 24);
    f->addRow(QStringLiteral("Resolución del FLAC:"), flacBits_);
    mp3Quality_ = new QComboBox;
    mp3Quality_->addItem(QStringLiteral("320 kbps CBR (máxima)"), 320);
    mp3Quality_->addItem(QStringLiteral("VBR V0 (~245 kbps, calidad casi igual, ocupa menos)"), 0);
    mp3Quality_->addItem(QStringLiteral("256 kbps CBR"), 256);
    mp3Quality_->addItem(QStringLiteral("192 kbps CBR"), 192);
    mp3Quality_->setToolTip(QStringLiteral("MP3 con LAME. Se graba a la misma frecuencia que el FLAC (máximo 48 kHz)."));
    f->addRow(QStringLiteral("Calidad del MP3:"), mp3Quality_);
    avOffset_ = new QSpinBox;
    avOffset_->setRange(-1000, 1000);
    avOffset_->setSingleStep(10);
    avOffset_->setSuffix(QStringLiteral(" ms"));
    avOffset_->setToolTip(QStringLiteral(
        "Positivo: retrasa el sonido respecto a la imagen (si en el directo las manos van por detrás del sonido).\n"
        "Negativo: retrasa la imagen. No afecta a las grabaciones FLAC/MP3."));
    f->addRow(QStringLiteral("Sincronía imagen/sonido:"), avOffset_);
    auto* note = new QLabel(QStringLiteral(
        "El nivel se ajusta en KDE (volumen de esta entrada). El vúmetro mide la señal digital: si el piloto SATURA se "
        "enciende, baja la entrada. Deja los picos por debajo de −3 dB. Ten en cuenta que si la distorsión ocurre "
        "antes del conversor (mesa o previo demasiado alto) también se oirá aunque el vúmetro no llegue arriba."));
    note->setWordWrap(true);
    f->addRow(note);

    connect(audioSrc_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!loading_) restartAudio();
    });
    connect(flacRate_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateAudioInfo(); });
    connect(flacBits_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateAudioInfo(); });
    connect(mp3Quality_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateAudioInfo(); });
    return w;
}

// ---------------------------------------------------------------------------
// Ajustes
// ---------------------------------------------------------------------------
static void selectData(QComboBox* c, const QVariant& v) {
    int i = c->findData(v);
    if (i >= 0) c->setCurrentIndex(i);
}

void MainWindow::loadSettings() {
    loading_ = true;
    QSettings s;
    twitchOn_->setChecked(s.value("twitch/enabled", false).toBool());
    twitchKey_->setText(s.value("twitch/key").toString());
    twitchServer_->setText(s.value("twitch/server").toString());
    twitchKbps_->setValue(s.value("twitch/kbps", 4500).toInt());
    youtubeOn_->setChecked(s.value("youtube/enabled", false).toBool());
    youtubeKey_->setText(s.value("youtube/key").toString());
    youtubeServer_->setText(s.value("youtube/server").toString());
    youtubeKbps_->setValue(s.value("youtube/kbps", 6000).toInt());
    customOn_->setChecked(s.value("custom/enabled", false).toBool());
    customUrl_->setText(s.value("custom/url").toString());
    customKbps_->setValue(s.value("custom/kbps", 6000).toInt());
    recordOn_->setChecked(s.value("record/enabled", true).toBool());
    recordMp3_->setChecked(s.value("record/mp3_enabled", false).toBool());
    selectData(mp3Quality_, s.value("record/mp3_kbps", 320).toInt());
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty()) music = QDir::homePath();
    flacDir_->setText(s.value("record/dir", music + "/Sesiones DJ").toString());
    selectData(flacRate_, s.value("record/rate", 0).toInt());
    selectData(flacBits_, s.value("record/bits", 16).toInt());
    selectData(audioKbps_, s.value("audio/kbps", 160).toInt());
    avOffset_->setValue(s.value("audio/av_offset_ms", 0).toInt());
    selectData(encoder_, s.value("video/encoder", "auto").toString());
    selectData(preset_, s.value("video/preset", "p5").toString());
    selectData(outRes_, s.value("video/output", "1280x720").toString());

    refreshCameras();
    selectData(camDev_, s.value("video/device").toString());
    refreshModes();
    selectData(camMode_, s.value("video/mode").toString());
    refreshAudioSources();
    QString src = s.value("audio/source").toString();
    if (!src.isEmpty()) selectData(audioSrc_, src);
    restoreGeometry(s.value("window/geometry").toByteArray());
    loading_ = false;
    updateAudioInfo();
}

void MainWindow::saveSettings() {
    QSettings s;
    s.setValue("twitch/enabled", twitchOn_->isChecked());
    s.setValue("twitch/key", twitchKey_->text().trimmed());
    s.setValue("twitch/server", twitchServer_->text().trimmed());
    s.setValue("twitch/kbps", twitchKbps_->value());
    s.setValue("youtube/enabled", youtubeOn_->isChecked());
    s.setValue("youtube/key", youtubeKey_->text().trimmed());
    s.setValue("youtube/server", youtubeServer_->text().trimmed());
    s.setValue("youtube/kbps", youtubeKbps_->value());
    s.setValue("custom/enabled", customOn_->isChecked());
    s.setValue("custom/url", customUrl_->text().trimmed());
    s.setValue("custom/kbps", customKbps_->value());
    s.setValue("record/enabled", recordOn_->isChecked());
    s.setValue("record/mp3_enabled", recordMp3_->isChecked());
    s.setValue("record/mp3_kbps", mp3Quality_->currentData());
    s.setValue("record/dir", flacDir_->text().trimmed());
    s.setValue("record/rate", flacRate_->currentData());
    s.setValue("record/bits", flacBits_->currentData());
    s.setValue("audio/kbps", audioKbps_->currentData());
    s.setValue("audio/av_offset_ms", avOffset_->value());
    s.setValue("audio/source", audioSrc_->currentData());
    s.setValue("video/encoder", encoder_->currentData());
    s.setValue("video/preset", preset_->currentData());
    s.setValue("video/output", outRes_->currentData());
    s.setValue("video/device", camDev_->currentData());
    s.setValue("video/mode", camMode_->currentData());
    s.setValue("window/geometry", saveGeometry());
    s.sync();
    // contiene las claves de emisión: sólo para el usuario
    QFile::setPermissions(s.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// ---------------------------------------------------------------------------
// Cámara
// ---------------------------------------------------------------------------
void MainWindow::refreshCameras() {
    bool old = loading_;
    loading_ = true;
    QString cur = camDev_->currentData().toString();
    cams_ = listCameras();
    camDev_->clear();
    for (auto& c : cams_)
        camDev_->addItem(QStringLiteral("%1 (%2)").arg(QString::fromStdString(c.name), QString::fromStdString(c.path)),
                         QString::fromStdString(c.path));
    if (cams_.empty()) camDev_->addItem(QStringLiteral("(no se ha encontrado ninguna cámara)"), QString());
    selectData(camDev_, cur);
    refreshModes();
    loading_ = old;
}

void MainWindow::refreshModes() {
    bool old = loading_;
    loading_ = true;
    QString cur = camMode_->currentData().toString();
    camMode_->clear();
    QString dev = camDev_->currentData().toString();
    for (auto& c : cams_) {
        if (QString::fromStdString(c.path) != dev) continue;
        // un modo por resolución+fps; si lo da en YUYV (sin comprimir) mejor que MJPEG
        std::map<std::tuple<int, int, int>, uint32_t> m;
        for (auto& md : c.modes) {
            auto k = std::make_tuple(md.w, md.h, md.fps);
            if (!m.count(k) || md.fourcc == V4L2_PIX_FMT_YUYV) m[k] = md.fourcc;
        }
        std::vector<std::tuple<int, int, int, uint32_t>> v;
        for (auto& [k, f] : m) v.emplace_back(std::get<0>(k), std::get<1>(k), std::get<2>(k), f);
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
            int pa = std::get<0>(a) * std::get<1>(a), pb = std::get<0>(b) * std::get<1>(b);
            return pa != pb ? pa > pb : std::get<2>(a) > std::get<2>(b);
        });
        for (auto& [w, h, fps, f] : v)
            camMode_->addItem(QStringLiteral("%1×%2 @ %3 fps (%4)").arg(w).arg(h).arg(fps).arg(QString::fromStdString(fourccName(f))),
                              QStringLiteral("%1x%2@%3/%4").arg(w).arg(h).arg(fps).arg(QString::fromStdString(fourccName(f))));
    }
    int idx = camMode_->findData(cur);
    if (idx < 0) idx = camMode_->findData(QStringLiteral("1280x720@60/MJPEG"));
    if (idx < 0) idx = camMode_->findData(QStringLiteral("1280x720@60/YUYV"));
    if (idx < 0)
        for (int i = 0; i < camMode_->count() && idx < 0; i++)
            if (camMode_->itemData(i).toString().startsWith("1280x720@")) idx = i;
    if (idx >= 0) camMode_->setCurrentIndex(idx);
    loading_ = old;
}

void MainWindow::restartCamera() {
    camera_.stop();
    preview_->clearImage(QStringLiteral("Sin cámara"));
    QString dev = camDev_->currentData().toString();
    QString mode = camMode_->currentData().toString();
    int w = 0, h = 0, fps = 0;
    char fcc[16] = {0};
    if (dev.isEmpty() || sscanf(mode.toUtf8().constData(), "%dx%d@%d/%15s", &w, &h, &fps, fcc) != 4) {
        preview_->clearImage(QStringLiteral("No se ha encontrado ninguna cámara.\nConéctala y pulsa ↻ en «Cámara y vídeo»."));
        return;
    }
    int ow = 0, oh = 0;
    sscanf(outRes_->currentData().toString().toUtf8().constData(), "%dx%d", &ow, &oh);
    if (ow <= 0 || oh <= 0) ow = w, oh = h;
    outW_ = ow & ~1;
    outH_ = oh & ~1;
    camFps_ = fps;
    CameraParams p;
    p.device = dev.toStdString();
    p.fourcc = std::string(fcc) == "YUYV" ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    p.w = w;
    p.h = h;
    p.fps = fps;
    p.outW = outW_;
    p.outH = outH_;
    lastFrameUs_ = 0;
    camera_.start(p, [this](const uint8_t* bgra, int w, int h, int stride, int64_t ts) {
        if (streamOn_) streamer_.pushVideo(bgra, stride, w, h, ts);
        lastFrameUs_ = nowUs();
        std::lock_guard<std::mutex> l(prevMtx_);
        if (prevNew_) return;   // la interfaz aún no ha pintado la anterior
        QImage img(w, h, QImage::Format_RGB32);   // en memoria: B,G,R,X = lo que entrega la cámara
        for (int y = 0; y < h; y++) memcpy(img.scanLine(y), bgra + (size_t)y * stride, (size_t)w * 4);
        prevImg_ = img;
        prevNew_ = true;
    });
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
const AudioSourceInfo* MainWindow::currentSource() const {
    std::string name = audioSrc_->currentData().toString().toStdString();
    for (auto& s : sources_)
        if (s.name == name) return &s;
    return nullptr;
}

void MainWindow::refreshAudioSources() {
    bool old = loading_;
    loading_ = true;
    QString cur = audioSrc_->currentData().toString();
    sources_ = listAudioSources();
    audioSrc_->clear();
    for (auto& s : sources_)
        audioSrc_->addItem(QString::fromStdString(s.description.empty() ? s.name : s.description),
                           QString::fromStdString(s.name));
    if (sources_.empty()) audioSrc_->addItem(QStringLiteral("(entrada predeterminada)"), QString());
    int idx = audioSrc_->findData(cur);
    if (idx < 0) idx = audioSrc_->findData(QString::fromStdString(defaultAudioSource()));
    if (idx >= 0) audioSrc_->setCurrentIndex(idx);
    loading_ = old;
}

void MainWindow::restartAudio() {
    const AudioSourceInfo* s = currentSource();
    int rate = s && s->rate > 0 ? s->rate : 48000;
    audio_.start(s ? s->name : std::string(), rate, [this](const float* in, int frames, int64_t ts) {
        if (streamOn_) streamer_.pushAudio(in, frames, ts);
        if (recOn_) {
            flac_.push(in, frames);   // cada grabador ignora si no está activo
            mp3_.push(in, frames);
        }
    });
    meter_->reset();
    updateAudioInfo();
}

void MainWindow::updateAudioInfo() {
    if (!audioInfo_) return;
    const AudioSourceInfo* s = currentSource();
    int rate = audio_.rate();
    int flacRate = flacRate_->currentData().toInt();
    if (!flacRate) flacRate = rate;
    int bits = flacBits_->currentData().toInt();
    QString t;
    if (s)
        t = QStringLiteral("La entrada trabaja en <b>%1, %2 canales, %3 Hz</b>. ")
                .arg(QString::fromStdString(s->format))
                .arg(s->channels)
                .arg(s->rate);
    int mp3Rate = (flacRate == 44100 || flacRate == 48000 || flacRate == 32000) ? flacRate : 48000;
    int mp3k = mp3Quality_->currentData().toInt();
    auto resampled = [&](int r) {
        return r != rate ? QStringLiteral(" <span style='color:#c80'>(remuestreado)</span>") : QStringLiteral(" (sin remuestrear)");
    };
    t += QStringLiteral("Se captura a %1 Hz. Directo: AAC a %2 Hz.<br>FLAC: %3 Hz / %4 bits")
             .arg(rate)
             .arg(rate == 44100 || rate == 48000 ? rate : 48000)
             .arg(flacRate)
             .arg(bits);
    t += resampled(flacRate);
    t += QStringLiteral(" · MP3: %1 Hz / %2").arg(mp3Rate).arg(mp3k ? QStringLiteral("%1 kbps").arg(mp3k) : QStringLiteral("VBR V0"));
    t += resampled(mp3Rate);
    if (s && bits == 24 && s->bits() <= 16)
        t += QStringLiteral("<br>La entrada es de 16 bits: grabar a 24 no aporta calidad, sólo ocupa más.");
    audioInfo_->setText(t);
}

// ---------------------------------------------------------------------------
// Empezar / parar
// ---------------------------------------------------------------------------
void MainWindow::onStartStop() {
    if (stopping_) return;
    if (live_) stopLive(true);
    else startLive();
}

static std::string joinUrl(QString server, const QString& def, const QString& key) {
    server = server.trimmed();
    if (server.isEmpty()) server = def;
    while (server.endsWith('/')) server.chop(1);
    return (server + "/" + key.trimmed()).toStdString();
}

void MainWindow::startLive() {
    saveSettings();
    const bool tw = twitchOn_->isChecked(), yt = youtubeOn_->isChecked(), cu = customOn_->isChecked();
    const bool recFlac = recordOn_->isChecked(), recMp3 = recordMp3_->isChecked();
    const bool rec = recFlac || recMp3;
    if (!tw && !yt && !cu && !rec) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("Activa al menos un destino o la grabación del audio."));
        return;
    }
    StreamConfig sc;
    if (tw) {
        if (twitchKey_->text().trimmed().isEmpty()) {
            tabs_->setCurrentWidget(tabDest_);
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Falta la clave de emisión de Twitch."));
            return;
        }
        sc.dests.push_back({"Twitch", joinUrl(twitchServer_->text(), kTwitchServer, twitchKey_->text()), twitchKbps_->value()});
    }
    if (yt) {
        if (youtubeKey_->text().trimmed().isEmpty()) {
            tabs_->setCurrentWidget(tabDest_);
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Falta la clave de emisión de YouTube."));
            return;
        }
        sc.dests.push_back({"YouTube", joinUrl(youtubeServer_->text(), kYouTubeServer, youtubeKey_->text()), youtubeKbps_->value()});
    }
    if (cu) {
        QString u = customUrl_->text().trimmed();
        if (u.isEmpty()) {
            tabs_->setCurrentWidget(tabDest_);
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Falta la URL o el archivo del destino personalizado."));
            return;
        }
        if (u.startsWith("~/")) u = QDir::homePath() + u.mid(1);
        sc.dests.push_back({"Personalizado", u.toStdString(), customKbps_->value()});
    }
    if (!sc.dests.empty() && lastFrameUs_ == 0 &&
        QMessageBox::question(this, windowTitle(),
                              QStringLiteral("La cámara no está enviando imagen. Se emitirá en negro hasta que funcione.\n"
                                             "¿Empezar igualmente?")) != QMessageBox::Yes)
        return;
    sc.width = outW_;
    sc.height = outH_;
    sc.fps = camFps_;
    sc.encoder = encoder_->currentData().toString().toStdString();
    sc.preset = preset_->currentData().toString().toStdString();
    sc.audioKbps = audioKbps_->currentData().toInt();
    sc.inputRate = audio_.rate();
    sc.avOffsetMs = avOffset_->value();

    if (rec) {
        QString dir = flacDir_->text().trimmed();
        if (dir.startsWith("~/")) dir = QDir::homePath() + dir.mid(1);
        if (dir.isEmpty() || !QDir().mkpath(dir)) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("No se puede crear la carpeta de grabación:\n%1").arg(dir));
            return;
        }
        // misma sesión, mismo nombre: djset_fecha.flac y djset_fecha.mp3
        QString base = dir + "/djset_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
        const AudioSourceInfo* s = currentSource();
        RecordOptions ro;
        ro.inRate = audio_.rate();
        ro.outRate = flacRate_->currentData().toInt();
        ro.bits = flacBits_->currentData().toInt();
        ro.inBits = s ? s->bits() : 32;
        ro.mp3Kbps = mp3Quality_->currentData().toInt();
        lastRecPaths_.clear();
        std::string err;
        bool ok = true;
        if (recFlac) {
            ro.format = RecordOptions::FLAC;
            ok = flac_.start((base + ".flac").toStdString(), ro, err);
            if (ok) lastRecPaths_ << base + ".flac";
        }
        if (ok && recMp3) {
            ro.format = RecordOptions::MP3;
            ok = mp3_.start((base + ".mp3").toStdString(), ro, err);
            if (ok) lastRecPaths_ << base + ".mp3";
        }
        if (!ok) {
            flac_.stop();
            mp3_.stop();
            QMessageBox::warning(this, windowTitle(), QStringLiteral("No se pudo empezar a grabar:\n%1").arg(QString::fromStdString(err)));
            return;
        }
        recOn_ = true;
    }
    if (!sc.dests.empty()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        std::string err;
        bool ok = streamer_.start(sc, err);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            recOn_ = false;
            flac_.stop();
            mp3_.stop();
            QMessageBox::warning(this, windowTitle(), QStringLiteral("No se pudo empezar el directo:\n%1").arg(QString::fromStdString(err)));
            return;
        }
        streamOn_ = true;
    }
    live_ = true;
    liveTimer_.start();
    setLiveUi(true);
    onStatusTimer();
}

void MainWindow::stopLive(bool ask) {
    if (!live_ || stopping_) return;
    if (ask && QMessageBox::question(this, windowTitle(), QStringLiteral("¿Terminar el directo y la grabación?"),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    stopping_ = true;
    startBtn_->setEnabled(false);
    startBtn_->setText(QStringLiteral("Parando…"));
    streamOn_ = false;
    recOn_ = false;
    // cerrar las conexiones puede tardar unos segundos: fuera del hilo de la interfaz
    std::thread([this] {
        streamer_.stop();
        flac_.stop();
        mp3_.stop();
        QMetaObject::invokeMethod(this, [this] {
            stopping_ = false;
            live_ = false;
            setLiveUi(false);
            onStatusTimer();
            if (!lastRecPaths_.isEmpty())
                statusRec_->setText(QStringLiteral("Grabación guardada: <b>%1</b>").arg(lastRecPaths_.join(QStringLiteral("  +  ")).toHtmlEscaped()));
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::setLiveUi(bool live) {
    for (QWidget* w : {tabDest_, tabVideo_, tabAudio_}) w->setEnabled(!live);
    for (auto* c : {twitchOn_, youtubeOn_, customOn_, recordOn_, recordMp3_}) c->setEnabled(!live);
    startBtn_->setEnabled(true);
    startBtn_->setText(live ? QStringLiteral("■  TERMINAR") : QStringLiteral("▶  EMPEZAR"));
    startBtn_->setStyleSheet(live ? QStringLiteral("QPushButton{background:#b00;color:white;}") : QString());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (stopping_) {
        e->ignore();
        return;
    }
    if (live_) {
        if (QMessageBox::question(this, windowTitle(), QStringLiteral("Se está emitiendo/grabando. ¿Terminar y salir?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        streamOn_ = false;
        recOn_ = false;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        streamer_.stop();
        flac_.stop();
        mp3_.stop();
        QApplication::restoreOverrideCursor();
        live_ = false;
    }
    saveSettings();
    e->accept();
}

// ---------------------------------------------------------------------------
// Temporizadores
// ---------------------------------------------------------------------------
QString MainWindow::elapsed() const { return fmtDuration(liveTimer_.isValid() ? liveTimer_.elapsed() : 0); }

void MainWindow::onPreviewTimer() {
    QImage img;
    {
        std::lock_guard<std::mutex> l(prevMtx_);
        if (prevNew_) {
            img = prevImg_;
            prevNew_ = false;
        }
    }
    if (!img.isNull()) preview_->setImage(img);
    else if (camera_.running() && (lastFrameUs_ == 0 || nowUs() - lastFrameUs_ > 1500000))
        preview_->clearImage(QString::fromStdString(camera_.status()));
}

void MainWindow::onMeterTimer() {
    float pk[2];
    bool cl[2];
    audio_.takeLevels(pk, cl);
    double dt = meterClock_.restart() / 1000.0;
    meter_->update(pk, cl, std::min(dt, 0.2));
}

void MainWindow::appendLog(const QString& line) { log_->appendPlainText(line); }

void MainWindow::onStatusTimer() {
    const bool streaming = live_ && streamOn_;
    const bool recording = live_ && (flac_.active() || mp3_.active());
    if (live_ && !stopping_) {
        QString what = streaming ? QStringLiteral("EN DIRECTO") : QStringLiteral("GRABANDO");
        statusLive_->setText(QStringLiteral("<span style='color:#d00'>●</span> %1 %2").arg(what, elapsed()));
        preview_->setLive(true, QStringLiteral("%1  %2").arg(what, elapsed()));
    } else if (!stopping_) {
        statusLive_->setText(QStringLiteral("Preparado"));
        preview_->setLive(false, QString());
    }

    if (streaming) {
        QStringList parts;
        for (auto& s : streamer_.status()) {
            QString color = s.live ? "#1a1" : "#c60";
            QString t = QStringLiteral("<b>%1</b>: <span style='color:%2'>%3</span>")
                            .arg(QString::fromStdString(s.name), color, QString::fromStdString(s.state).toHtmlEscaped());
            if (s.live) t += QStringLiteral(" · %1 kbps").arg((int)s.kbps);
            if (s.reconnects) t += QStringLiteral(" · %1 reconexión(es)").arg(s.reconnects);
            parts << t;
        }
        parts << QStringLiteral("%1 · %2 fps").arg(QString::fromStdString(streamer_.encoderName())).arg(streamer_.encodeFps(), 0, 'f', 1);
        statusDest_->setText(parts.join(QStringLiteral(" &nbsp;|&nbsp; ")));
    } else if (!live_) {
        statusDest_->clear();
    }

    if (recording) {
        QStringList parts;
        std::string anyPath;
        for (AudioRecorder* r : {&flac_, &mp3_}) {
            if (!r->active()) continue;
            anyPath = r->path();
            QString t = QStringLiteral("%1: <b>%2</b> · %3 · %4 MB")
                            .arg(QString::fromLatin1(r->formatName()),
                                 QFileInfo(QString::fromStdString(r->path())).fileName().toHtmlEscaped(),
                                 fmtDuration((qint64)(r->seconds() * 1000)))
                            .arg(r->bytes() / 1048576.0, 0, 'f', 1);
            std::string e = r->error();
            if (!e.empty()) t += QStringLiteral(" · <span style='color:#d00'><b>%1</b></span>").arg(QString::fromStdString(e).toHtmlEscaped());
            parts << t;
        }
        QString t = parts.join(QStringLiteral(" &nbsp;|&nbsp; "));
        struct statvfs sv;
        if (statvfs(QFileInfo(QString::fromStdString(anyPath)).absolutePath().toUtf8().constData(), &sv) == 0) {
            double freeGb = (double)sv.f_bavail * sv.f_frsize / 1073741824.0;
            t += QStringLiteral(" · libres %1 GB").arg(freeGb, 0, 'f', 1);
            if (freeGb < 2) t += QStringLiteral(" <span style='color:#d00'><b>¡poco espacio!</b></span>");
        }
        statusRec_->setText(t);
    } else if (live_ && !stopping_) {
        statusRec_->setText(QStringLiteral("Sin grabación de audio"));
    } else if (!live_ && lastRecPaths_.isEmpty()) {
        statusRec_->clear();
    }

    QString cam = QString::fromStdString(camera_.status()).toHtmlEscaped();
    if (camera_.fps() > 0) cam += QStringLiteral(" · <b>%1 fps</b>").arg(camera_.fps(), 0, 'f', 1);
    if (camera_.fps() > 0 && camera_.fps() < camFps_ * 0.85)
        cam += QStringLiteral(" <span style='color:#c60'>(menos de lo pedido: ¿poca luz?)</span>");
    statusInfo_->setText(QStringLiteral("<span style='color:gray'>Cámara: %1<br>Audio: %2</span>")
                             .arg(cam, QString::fromStdString(audio_.status()).toHtmlEscaped()));
}
