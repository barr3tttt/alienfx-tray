#pragma once

#include <QObject>
#include <QSystemTrayIcon>

#include "app/metatypes.h"

class QAction;
class QActionGroup;
class QMenu;

namespace alienfx {
class LightingController;
}

namespace alienfx::ui {

class ControlPanel;

// Left click opens the control panel, right click gives schemes and an on/off toggle.
//
// On Plasma 6 under Wayland this reaches the panel through StatusNotifierItem, which
// QSystemTrayIcon speaks natively - there is no legacy XEmbed tray involved and no KF6
// dependency needed to get a well-behaved icon.
class TrayIcon : public QObject {
    Q_OBJECT

   public:
    explicit TrayIcon(LightingController& controller, QObject* parent = nullptr);
    ~TrayIcon() override;

    [[nodiscard]] bool isSystemTrayAvailable() const;
    void show();

    void togglePanel();

   private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);
    void rebuildSchemeActions();
    void onStatusChanged(alienfx::Domain domain, alienfx::DomainStatus status);

   private:
    void updateTooltip();

    LightingController& controller_;
    QSystemTrayIcon* icon_ = nullptr;
    QMenu* menu_ = nullptr;
    QMenu* schemeMenu_ = nullptr;
    QActionGroup* schemeGroup_ = nullptr;
    QAction* lightsAction_ = nullptr;
    ControlPanel* panel_ = nullptr;
};

}  // namespace alienfx::ui
