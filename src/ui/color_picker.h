#pragma once

#include <QColor>
#include <QImage>
#include <QWidget>

class QLineEdit;

namespace alienfx::ui {

// Hue around the circumference, saturation along the radius. Emits on every mouse move
// while the button is down - live preview is the point of this widget, not an afterthought.
class ColorWheel : public QWidget {
    Q_OBJECT

   public:
    explicit ColorWheel(QWidget* parent = nullptr);

    [[nodiscard]] QColor color() const { return color_; }
    void setColor(const QColor& color);

    [[nodiscard]] QSize sizeHint() const override { return {180, 180}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {120, 120}; }

   signals:
    void colorChanged(const QColor& color);
    void interactionFinished();

   protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

   private:
    void rebuildWheel();
    void pick(const QPointF& position);
    [[nodiscard]] QRectF wheelRect() const;

    QImage wheel_;
    QColor color_{Qt::white};
    bool dragging_ = false;
};

// The value axis the wheel does not cover: black through to the wheel's fully saturated
// colour. Painted as the actual gradient so the choice is visible rather than numeric.
class ShadeBar : public QWidget {
    Q_OBJECT

   public:
    explicit ShadeBar(QWidget* parent = nullptr);

    void setHueSaturation(const QColor& color);
    [[nodiscard]] qreal value() const { return value_; }
    void setValue(qreal value);

    [[nodiscard]] QSize sizeHint() const override { return {180, 22}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {80, 18}; }

   signals:
    void valueChanged(qreal value);
    void interactionFinished();

   protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

   private:
    void pick(const QPointF& position);

    QColor base_{Qt::white};  // hue and saturation at full value
    qreal value_ = 1.0;
    bool dragging_ = false;
};

// Wheel plus shade bar plus a hex field, kept in sync. colorChanged fires continuously
// during a drag; colorCommitted fires once the user lets go, for anything that should only
// happen on a settled value.
class ColorPicker : public QWidget {
    Q_OBJECT

   public:
    explicit ColorPicker(QWidget* parent = nullptr);

    [[nodiscard]] QColor color() const { return color_; }
    void setColor(const QColor& color);

   signals:
    void colorChanged(const QColor& color);
    void colorCommitted(const QColor& color);

   private:
    void applyColor(const QColor& color, bool fromHexField);

    ColorWheel* wheel_ = nullptr;
    ShadeBar* shade_ = nullptr;
    QLineEdit* hex_ = nullptr;
    QColor color_{Qt::white};
    bool updating_ = false;
};

}  // namespace alienfx::ui
