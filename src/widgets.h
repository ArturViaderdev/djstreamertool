#pragma once
#include <QImage>
#include <QString>
#include <QWidget>

// Vista previa de la cámara (tal cual sale en el directo: sin espejo)
class PreviewWidget : public QWidget {
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    void clearImage(const QString& msg);
    void setLive(bool live, const QString& badge);
    QSize sizeHint() const override { return {800, 450}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QImage img_;
    QString msg_;
    bool live_ = false;
    QString badge_;
};

// Vúmetro estéreo de picos con retención, pico máximo y piloto de saturación
// (se queda encendido hasta que se hace clic sobre él).
class LevelMeter : public QWidget {
public:
    explicit LevelMeter(QWidget* parent = nullptr);
    // peak: pico lineal de cada canal desde la última llamada; dt en segundos
    void update(const float peak[2], const bool clip[2], double dt);
    void reset();
    QSize sizeHint() const override { return {150, 450}; }
    QSize minimumSizeHint() const override { return {130, 260}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    float level_[2] = {-100, -100};   // dB mostrados (caída suave)
    float hold_[2] = {-100, -100};    // retención de pico
    double holdAge_[2] = {0, 0};
    float max_[2] = {-100, -100};     // máximo desde el último reinicio
    bool clip_[2] = {false, false};
    int clipCount_ = 0;
    bool lastClip_[2] = {false, false};
};
