#include "widgets.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// PreviewWidget
// ---------------------------------------------------------------------------
PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setAttribute(Qt::WA_OpaquePaintEvent);
    msg_ = QStringLiteral("Sin cámara");
}

void PreviewWidget::setImage(const QImage& img) {
    img_ = img;
    msg_.clear();
    QWidget::update();
}

void PreviewWidget::clearImage(const QString& msg) {
    img_ = QImage();
    msg_ = msg;
    QWidget::update();
}

void PreviewWidget::setLive(bool live, const QString& badge) {
    live_ = live;
    badge_ = badge;
    QWidget::update();
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!img_.isNull()) {
        QSize s = img_.size().scaled(size(), Qt::KeepAspectRatio);
        QRect r(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
        p.setRenderHint(QPainter::SmoothPixmapTransform, s.width() < img_.width());
        p.drawImage(r, img_);
    } else {
        p.setPen(QColor(200, 200, 200));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.2);
        p.setFont(f);
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, msg_);
    }
    if (!badge_.isEmpty()) {
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 1.15);
        p.setFont(f);
        QFontMetrics fm(f);
        QRect tr = fm.boundingRect(badge_);
        QRect box(12, 12, tr.width() + 34, fm.height() + 12);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(live_ ? QColor(200, 0, 0, 220) : QColor(40, 40, 40, 200));
        p.drawRoundedRect(box, 6, 6);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(box.left() + 14, box.center().y() + 1), 5, 5);
        p.setPen(Qt::white);
        p.drawText(box.adjusted(26, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, badge_);
    }
}

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------
// Escala IEC 60268-18 (la de los medidores de JACK/Ardour): más resolución arriba
static double iecScale(double db) {
    double def;
    if (db < -70.0) def = 0.0;
    else if (db < -60.0) def = (db + 70.0) * 0.25;
    else if (db < -50.0) def = (db + 60.0) * 0.5 + 2.5;
    else if (db < -40.0) def = (db + 50.0) * 0.75 + 7.5;
    else if (db < -30.0) def = (db + 40.0) * 1.5 + 15.0;
    else if (db < -20.0) def = (db + 30.0) * 2.0 + 30.0;
    else if (db < 0.0) def = (db + 20.0) * 2.5 + 50.0;
    else def = 100.0;
    return def / 100.0;
}

static double iecInverse(double frac) {
    double d = frac * 100.0;
    if (d < 2.5) return d / 0.25 - 70.0;
    if (d < 7.5) return (d - 2.5) / 0.5 - 60.0;
    if (d < 15.0) return (d - 7.5) / 0.75 - 50.0;
    if (d < 30.0) return (d - 15.0) / 1.5 - 40.0;
    if (d < 50.0) return (d - 30.0) / 2.0 - 30.0;
    return (d - 50.0) / 2.5 - 20.0;
}

static QColor zoneColor(double db, bool lit) {
    QColor c;
    if (db >= -1.0) c = QColor(255, 40, 30);         // a punto de saturar
    else if (db >= -6.0) c = QColor(255, 150, 0);    // poco margen
    else if (db >= -18.0) c = QColor(230, 220, 0);   // nivel fuerte
    else c = QColor(40, 210, 70);                    // normal
    if (!lit) c = c.darker(650);
    return c;
}

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Vúmetro de picos de la entrada (dBFS).\n"
                              "El piloto SATURA se queda encendido hasta que hagas clic aquí."));
}

void LevelMeter::reset() {
    for (int c = 0; c < 2; c++) {
        clip_[c] = false;
        max_[c] = -100;
        hold_[c] = level_[c];
    }
    clipCount_ = 0;
    QWidget::update();
}

void LevelMeter::mousePressEvent(QMouseEvent*) { reset(); }

void LevelMeter::update(const float peak[2], const bool clip[2], double dt) {
    for (int c = 0; c < 2; c++) {
        float db = peak[c] > 1e-6f ? 20.f * std::log10(peak[c]) : -120.f;
        // ataque instantáneo, caída de 20 dB/s
        level_[c] = std::max(db, level_[c] - (float)(20.0 * dt));
        if (db >= hold_[c]) {
            hold_[c] = db;
            holdAge_[c] = 0;
        } else if ((holdAge_[c] += dt) > 1.5) {
            hold_[c] = std::max(level_[c], hold_[c] - (float)(30.0 * dt));
        }
        max_[c] = std::max(max_[c], db);
        if (clip[c]) {
            clip_[c] = true;
            if (!lastClip_[c]) clipCount_++;
        }
        lastClip_[c] = clip[c];
    }
    QWidget::update();
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 26));
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.8));
    QFontMetrics fms(small);
    QFont bold = font();
    bold.setBold(true);

    const int lampH = 28, bottomH = 2 * fms.height() + 14, scaleW = fms.horizontalAdvance("-60") + 10;
    const int top = 8 + lampH + 8, bottom = height() - bottomH;
    const int barsW = width() - scaleW - 16;
    const int barW = std::max(10, (barsW - 6) / 2);
    const int x0 = scaleW + 8;
    const int h = std::max(10, bottom - top);
    auto yOf = [&](double db) { return top + (int)std::lround((1.0 - iecScale(db)) * h); };

    // Pilotos de saturación
    for (int c = 0; c < 2; c++) {
        QRect lamp(x0 + c * (barW + 6), 8, barW, lampH);
        p.setPen(Qt::NoPen);
        p.setBrush(clip_[c] ? QColor(255, 20, 20) : QColor(70, 10, 10));
        p.drawRoundedRect(lamp, 4, 4);
        p.setPen(clip_[c] ? Qt::white : QColor(150, 90, 90));
        p.setFont(small);
        p.drawText(lamp, Qt::AlignCenter, clip_[c] ? QStringLiteral("SATURA") : (c ? QStringLiteral("R") : QStringLiteral("L")));
    }

    // Escala
    p.setFont(small);
    const double ticks[] = {0, -3, -6, -9, -12, -18, -24, -30, -40, -50, -60};
    for (double t : ticks) {
        int y = yOf(t);
        p.setPen(QColor(150, 150, 155));
        p.drawText(QRect(0, y - fms.height() / 2, scaleW, fms.height()), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number((int)t));
        p.setPen(QColor(60, 60, 65));
        p.drawLine(scaleW + 2, y, width() - 4, y);
    }

    // Barras por segmentos (tipo LED)
    const int seg = 4, gap = 1;
    for (int c = 0; c < 2; c++) {
        const int bx = x0 + c * (barW + 6);
        const int litTop = yOf(level_[c]);
        for (int y = bottom - seg; y >= top; y -= seg + gap) {
            // dB del centro del segmento
            double frac = 1.0 - (double)(y + seg / 2 - top) / h;
            double db = iecInverse(frac);
            bool lit = y + seg / 2 >= litTop && level_[c] > -70;
            p.fillRect(bx, y, barW, seg, zoneColor(db, lit));
        }
        if (hold_[c] > -70) {
            int hy = yOf(std::min(0.f, hold_[c]));
            p.fillRect(bx, hy - 1, barW, 3, zoneColor(hold_[c], true).lighter(130));
        }
    }

    // Pico máximo de cada canal y saturaciones
    p.setFont(small);
    int ty = bottom + 6;
    for (int c = 0; c < 2; c++) {
        const int bx = x0 + c * (barW + 6);
        QString t = max_[c] <= -99 ? QStringLiteral("—") : QString::number(max_[c], 'f', 1);
        p.setPen(max_[c] >= -1.0 ? QColor(255, 80, 70) : QColor(220, 220, 225));
        p.drawText(QRect(bx - 4, ty, barW + 8, fms.height()), Qt::AlignCenter, t);
    }
    p.setPen(QColor(150, 150, 155));
    p.drawText(QRect(0, ty, scaleW, fms.height()), Qt::AlignRight, QStringLiteral("máx"));
    ty += fms.height() + 2;
    p.setPen(clipCount_ ? QColor(255, 80, 70) : QColor(120, 120, 125));
    p.drawText(QRect(0, ty, width(), fms.height()), Qt::AlignCenter,
               clipCount_ ? QStringLiteral("%1 saturación(es)").arg(clipCount_) : QStringLiteral("sin saturar"));
}
