#include "ui/tray_icon.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QIcon>
#include <QMenu>

#include "app/lighting_controller.h"
#include "ui/control_panel.h"

namespace alienfx::ui {
namespace {

QIcon trayIcon() {
    // Prefer a themed name so the icon follows the user's Plasma icon theme, and fall back
    // to the one shipped in the binary's resources when the theme has nothing suitable.
    QIcon icon = QIcon::fromTheme(QStringLiteral("alienfx-tray"));
    if (icon.isNull()) icon = QIcon(QStringLiteral(":/icons/alienfx-tray.svg"));
    if (icon.isNull()) icon = QIcon::fromTheme(QStringLiteral("preferences-desktop-color"));
    return icon;
}

}  // namespace

TrayIcon::TrayIcon(LightingController& controller, QObject* parent)
    : QObject(parent), controller_(controller) {
    icon_ = new QSystemTrayIcon(trayIcon(), this);
    menu_ = new QMenu;

    schemeMenu_ = menu_->addMenu(tr("Schemes"));
    schemeGroup_ = new QActionGroup(this);
    schemeGroup_->setExclusive(true);

    menu_->addSeparator();

    lightsAction_ = menu_->addAction(tr("Lights on"));
    lightsAction_->setCheckable(true);
    lightsAction_->setChecked(controller_.lightsOn());
    connect(lightsAction_, &QAction::toggled, &controller_, &LightingController::setLightsOn);

    QAction* openPanel = menu_->addAction(tr("Open control panel..."));
    connect(openPanel, &QAction::triggered, this, &TrayIcon::togglePanel);

    menu_->addSeparator();
    QAction* quit = menu_->addAction(tr("Quit"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    icon_->setContextMenu(menu_);

    connect(icon_, &QSystemTrayIcon::activated, this, &TrayIcon::onActivated);
    connect(&controller_, &LightingController::schemeListChanged, this,
            &TrayIcon::rebuildSchemeActions);
    // Activating a scheme only moves the tick. Rebuilding here would delete the very action
    // whose triggered() is still on the stack - and under Plasma that stack belongs to the
    // StatusNotifierItem DBus adaptor, which goes on to use the action after the slot returns.
    connect(&controller_, &LightingController::schemeChanged, this,
            &TrayIcon::updateSchemeChecks);
    connect(&controller_, &LightingController::lightsOnChanged, this, [this](bool on) {
        QSignalBlocker blocker(lightsAction_);
        lightsAction_->setChecked(on);
    });
    connect(&controller_, &LightingController::statusChanged, this, &TrayIcon::onStatusChanged);

    rebuildSchemeActions();
    updateTooltip();
}

TrayIcon::~TrayIcon() {
    delete panel_;
    delete menu_;
}

bool TrayIcon::isSystemTrayAvailable() const { return QSystemTrayIcon::isSystemTrayAvailable(); }

void TrayIcon::show() { icon_->show(); }

void TrayIcon::rebuildSchemeActions() {
    for (QAction* action : schemeMenu_->actions()) {
        schemeGroup_->removeAction(action);
        schemeMenu_->removeAction(action);
        action->deleteLater();
    }

    for (const Scheme& scheme : controller_.config().schemes) {
        const QString name = QString::fromStdString(scheme.name);
        QAction* action = schemeMenu_->addAction(name);
        action->setCheckable(true);
        // The scheme name, not the label: a menu label carries accelerator markup.
        action->setData(name);
        schemeGroup_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, name] { controller_.activateScheme(name); });
    }
    updateSchemeChecks();
}

void TrayIcon::updateSchemeChecks() {
    const QString active = QString::fromStdString(controller_.config().activeSchemeName);
    for (QAction* action : schemeMenu_->actions())
        action->setChecked(action->data().toString() == active);
    updateTooltip();
}

void TrayIcon::onActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) togglePanel();
}

void TrayIcon::togglePanel() {
    if (!panel_) panel_ = new ControlPanel(controller_);

    if (panel_->isVisible()) {
        panel_->hide();
        return;
    }
    panel_->refreshFromController();
    panel_->show();
    panel_->raise();
    panel_->activateWindow();
}

void TrayIcon::onStatusChanged(Domain, DomainStatus) { updateTooltip(); }

void TrayIcon::updateTooltip() {
    QStringList lines;
    lines << tr("AlienFX Lighting - %1")
                 .arg(QString::fromStdString(controller_.config().activeSchemeName));
    for (Domain domain : kAllDomains)
        lines << QString::fromStdString(describe(domain, controller_.status(domain)));
    icon_->setToolTip(lines.join('\n'));
}

}  // namespace alienfx::ui
