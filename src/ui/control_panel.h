#pragma once

#include <QWidget>

#include "app/metatypes.h"
#include "core/scheme.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace alienfx {
class LightingController;
}

namespace alienfx::ui {

class ColorPicker;

// The panel behind the tray icon's left click. One tab per lighting domain, because the two
// controllers really are independent, plus schemes and the login integration.
//
// Every widget change goes straight to the controller, which submits it to the device worker
// and returns immediately. Nothing in here ever waits on USB.
class ControlPanel : public QWidget {
    Q_OBJECT

   public:
    explicit ControlPanel(LightingController& controller, QWidget* parent = nullptr);

   public slots:
    void refreshFromController();

   private slots:
    void onStatusChanged(alienfx::Domain domain, alienfx::DomainStatus status);

   private:
    QWidget* buildKeyboardTab();
    QWidget* buildExternalTab();
    QWidget* buildSchemesTab();

    void pushScheme();
    void refreshZoneEditor();
    void refreshSchemeList();
    void refreshActiveSchemeMarker();
    void refreshDeleteButton();
    void refreshStartupIntegration();
    [[nodiscard]] int selectedZoneIndex() const;

    LightingController& controller_;
    Scheme working_;
    bool loading_ = false;

    // Keyboard tab
    ColorPicker* keyboardColor_ = nullptr;
    QSlider* keyboardBrightness_ = nullptr;
    QLabel* keyboardBrightnessValue_ = nullptr;
    QComboBox* keyboardEffect_ = nullptr;

    // External tab
    QListWidget* zoneList_ = nullptr;
    ColorPicker* zoneColor_ = nullptr;
    ColorPicker* zoneBatteryColor_ = nullptr;
    QCheckBox* zoneSeparateBattery_ = nullptr;
    QSlider* externalBrightness_ = nullptr;
    QLabel* externalBrightnessValue_ = nullptr;
    QPushButton* applyToAllZones_ = nullptr;

    // Schemes tab
    QListWidget* schemeList_ = nullptr;
    QPushButton* removeScheme_ = nullptr;
    QLabel* startupStatus_ = nullptr;
    QPushButton* startupToggle_ = nullptr;

    // Status strip
    QLabel* keyboardStatus_ = nullptr;
    QLabel* externalStatus_ = nullptr;
};

}  // namespace alienfx::ui
