#include "ui/color_picker.h"

#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QtMath>

namespace alienfx::ui {
namespace {

constexpr int kHandleRadius = 6;

qreal clamp01(qreal value) { return qBound(qreal(0), value, qreal(1)); }

}  // namespace

// ------------------------------------------------------------------------------ wheel ---

ColorWheel::ColorWheel(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::ClickFocus);
}

QRectF ColorWheel::wheelRect() const {
    const qreal side = qMin(width(), height()) - 2.0 * kHandleRadius;
    return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
}

void ColorWheel::setColor(const QColor& color) {
    if (color_ == color) return;
    color_ = color;
    update();
}

void ColorWheel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    rebuildWheel();
}

void ColorWheel::rebuildWheel() {
    const QRectF rect = wheelRect();
    const int side = qMax(1, qRound(rect.width()));
    const qreal dpr = devicePixelRatioF();
    const int pixels = qMax(1, qRound(side * dpr));

    wheel_ = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    wheel_.fill(Qt::transparent);
    wheel_.setDevicePixelRatio(dpr);

    const qreal radius = pixels / 2.0;
    for (int y = 0; y < pixels; ++y) {
        auto* line = reinterpret_cast<QRgb*>(wheel_.scanLine(y));
        const qreal dy = y + 0.5 - radius;
        for (int x = 0; x < pixels; ++x) {
            const qreal dx = x + 0.5 - radius;
            const qreal distance = std::hypot(dx, dy) / radius;
            if (distance > 1.0) continue;

            const qreal hue = std::fmod(qRadiansToDegrees(std::atan2(-dy, dx)) + 360.0, 360.0);
            QColor pixel = QColor::fromHsvF(hue / 360.0, clamp01(distance), 1.0);

            // Feather the last pixel of the rim so the circle does not look jagged.
            const qreal edge = clamp01((1.0 - distance) * radius);
            pixel.setAlphaF(edge);
            line[x] = pixel.rgba();
        }
    }
}

void ColorWheel::paintEvent(QPaintEvent*) {
    if (wheel_.isNull()) rebuildWheel();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect = wheelRect();
    painter.drawImage(rect.topLeft(), wheel_);

    // The selection marker sits where the current hue and saturation put it.
    const qreal hue = color_.hueF() < 0 ? 0.0 : color_.hueF();
    const qreal saturation = clamp01(color_.hsvSaturationF());
    const qreal angle = qDegreesToRadians(hue * 360.0);
    const qreal radius = rect.width() / 2.0;
    const QPointF centre = rect.center();
    const QPointF handle(centre.x() + std::cos(angle) * saturation * radius,
                         centre.y() - std::sin(angle) * saturation * radius);

    painter.setPen(QPen(Qt::black, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(handle, kHandleRadius, kHandleRadius);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawEllipse(handle, kHandleRadius - 1.5, kHandleRadius - 1.5);
}

void ColorWheel::pick(const QPointF& position) {
    const QRectF rect = wheelRect();
    const QPointF centre = rect.center();
    const qreal radius = rect.width() / 2.0;
    if (radius <= 0) return;

    const qreal dx = position.x() - centre.x();
    const qreal dy = centre.y() - position.y();

    const qreal hue = std::fmod(qRadiansToDegrees(std::atan2(dy, dx)) + 360.0, 360.0);
    const qreal saturation = clamp01(std::hypot(dx, dy) / radius);

    QColor picked = QColor::fromHsvF(hue / 360.0, saturation, color_.valueF());
    if (picked == color_) return;

    color_ = picked;
    update();
    emit colorChanged(color_);
}

void ColorWheel::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    pick(event->position());
}

void ColorWheel::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    // Every move, not every Nth: the device worker coalesces, so the UI stays honest about
    // where the cursor is and the hardware catches up as fast as it can.
    pick(event->position());
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    emit interactionFinished();
}

// --------------------------------------------------------------------------- shade bar ---

ShadeBar::ShadeBar(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::ClickFocus);
}

void ShadeBar::setHueSaturation(const QColor& color) {
    QColor base = QColor::fromHsvF(color.hueF() < 0 ? 0.0 : color.hueF(),
                                   color.hsvSaturationF(), 1.0);
    if (base == base_) return;
    base_ = base;
    update();
}

void ShadeBar::setValue(qreal value) {
    const qreal clamped = clamp01(value);
    if (qFuzzyCompare(clamped + 1.0, value_ + 1.0)) return;
    value_ = clamped;
    update();
}

void ShadeBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = rect().adjusted(kHandleRadius, 2, -kHandleRadius, -2);

    QLinearGradient gradient(track.topLeft(), track.topRight());
    gradient.setColorAt(0.0, Qt::black);
    gradient.setColorAt(1.0, base_);

    QPainterPath path;
    path.addRoundedRect(track, 4, 4);
    painter.fillPath(path, gradient);
    painter.setPen(QPen(palette().mid().color(), 1));
    painter.drawPath(path);

    const qreal x = track.left() + value_ * track.width();
    painter.setPen(QPen(Qt::black, 1.5));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QPointF(x, track.center().y()), kHandleRadius, kHandleRadius);
}

void ShadeBar::pick(const QPointF& position) {
    const QRectF track = rect().adjusted(kHandleRadius, 0, -kHandleRadius, 0);
    if (track.width() <= 0) return;

    const qreal picked = clamp01((position.x() - track.left()) / track.width());
    if (qFuzzyCompare(picked + 1.0, value_ + 1.0)) return;

    value_ = picked;
    update();
    emit valueChanged(value_);
}

void ShadeBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    dragging_ = true;
    pick(event->position());
}

void ShadeBar::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) return;
    pick(event->position());
}

void ShadeBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    emit interactionFinished();
}

// ----------------------------------------------------------------------------- picker ---

ColorPicker::ColorPicker(QWidget* parent) : QWidget(parent) {
    wheel_ = new ColorWheel(this);
    shade_ = new ShadeBar(this);
    hex_ = new QLineEdit(this);

    hex_->setMaxLength(7);
    hex_->setAlignment(Qt::AlignCenter);
    hex_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^#?[0-9A-Fa-f]{0,6}$")), hex_));
    hex_->setToolTip(tr("Hex colour, for example #bd93f9"));

    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(wheel_, 0, 0, 1, 2);
    layout->addWidget(shade_, 1, 0, 1, 2);
    layout->addWidget(new QLabel(tr("Hex"), this), 2, 0);
    layout->addWidget(hex_, 2, 1);
    layout->setColumnStretch(1, 1);

    connect(wheel_, &ColorWheel::colorChanged, this,
            [this](const QColor& color) { applyColor(color, false); });
    connect(shade_, &ShadeBar::valueChanged, this, [this](qreal value) {
        QColor next = QColor::fromHsvF(color_.hueF() < 0 ? 0.0 : color_.hueF(),
                                       color_.hsvSaturationF(), value);
        applyColor(next, false);
    });
    connect(hex_, &QLineEdit::textEdited, this, [this](const QString& text) {
        QString normalised = text.startsWith('#') ? text : '#' + text;
        QColor parsed(normalised);
        if (parsed.isValid()) applyColor(parsed, true);
    });

    connect(wheel_, &ColorWheel::interactionFinished, this,
            [this] { emit colorCommitted(color_); });
    connect(shade_, &ShadeBar::interactionFinished, this,
            [this] { emit colorCommitted(color_); });
    connect(hex_, &QLineEdit::editingFinished, this, [this] { emit colorCommitted(color_); });

    setColor(color_);
}

void ColorPicker::setColor(const QColor& color) {
    if (!color.isValid()) return;
    color_ = color;

    updating_ = true;
    wheel_->setColor(color_);
    shade_->setHueSaturation(color_);
    shade_->setValue(color_.valueF());
    hex_->setText(color_.name(QColor::HexRgb));
    updating_ = false;
}

void ColorPicker::applyColor(const QColor& color, bool fromHexField) {
    if (updating_ || !color.isValid()) return;
    color_ = color;

    updating_ = true;
    wheel_->setColor(color_);
    shade_->setHueSaturation(color_);
    shade_->setValue(color_.valueF());
    if (!fromHexField) hex_->setText(color_.name(QColor::HexRgb));
    updating_ = false;

    emit colorChanged(color_);
}

}  // namespace alienfx::ui
