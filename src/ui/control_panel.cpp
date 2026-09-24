#include "ui/control_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QVBoxLayout>

#include "app/lighting_controller.h"
#include "app/startup_integration.h"
#include "ui/color_picker.h"
#include "ui/qt_color.h"

namespace alienfx::ui {
namespace {

// Holds the panel's write-back suppressed while a refresh populates the widgets, and puts
// the flag back the way it found it. Nestable on purpose: zoneList_->clear() emits
// currentRowChanged, which refreshes the zone editor from inside an outer refresh, and an
// inner scope ending must not re-enable edit propagation the outer one is relying on.
class LoadingScope {
   public:
    explicit LoadingScope(bool& flag) : flag_(flag), previous_(flag) { flag_ = true; }
    ~LoadingScope() { flag_ = previous_; }

    LoadingScope(const LoadingScope&) = delete;
    LoadingScope& operator=(const LoadingScope&) = delete;

   private:
    bool& flag_;
    bool previous_;
};

// Scheme list items show "name  (active)" but must still report the real name.
QString schemeNameOf(const QListWidgetItem* item) {
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString statusStyle(const DomainStatus& status) {
    // Orthogonal to the state, and more consequential than most of it. The lighting can have
    // worked perfectly and the USB interface still not have gone back to its kernel driver -
    // on this hardware that interface is the built-in keyboard, and nothing is retrying it,
    // so the row must not read like the ordinary success it otherwise is.
    if (status.interfaceLeftDetached) return QStringLiteral("font-weight: bold;");

    switch (status.state) {
        case DeviceState::Ready:
            return QStringLiteral("color: palette(text);");
        // Waiting resolves itself - the worker is holding the frame and re-trying - so it
        // does not need the user's attention. Busy does: nothing is re-trying it, and the
        // usual cause is a udev rule the user has to go and add.
        case DeviceState::Waiting:
        case DeviceState::Unknown:
            return QStringLiteral("color: palette(mid);");
        case DeviceState::Busy:
        case DeviceState::Missing:
        case DeviceState::Error:
        case DeviceState::Unresponsive:
            break;
    }
    return QStringLiteral("font-weight: bold;");
}

}  // namespace

ControlPanel::ControlPanel(LightingController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setWindowTitle(tr("AlienFX Lighting"));
    setWindowFlag(Qt::Dialog);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildKeyboardTab(), tr("Keyboard lighting"));
    tabs->addTab(buildExternalTab(), tr("External"));
    tabs->addTab(buildSchemesTab(), tr("Schemes"));
    tabs->setCurrentIndex(0);

    configStatus_ = new QLabel(this);
    keyboardStatus_ = new QLabel(this);
    externalStatus_ = new QLabel(this);
    for (QLabel* label : {configStatus_, keyboardStatus_, externalStatus_}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    // The config problem gets a line of its own. Sharing the keyboard's meant the next
    // device status update erased the only notice anywhere that the user's saved schemes
    // are not the ones being painted.
    configStatus_->setStyleSheet(QStringLiteral("font-weight: bold;"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(configStatus_);
    layout->addWidget(keyboardStatus_);
    layout->addWidget(externalStatus_);

    connect(&controller_, &LightingController::statusChanged, this,
            &ControlPanel::onStatusChanged);
    connect(&controller_, &LightingController::schemeChanged, this,
            &ControlPanel::refreshFromController);
    // Activating a scheme moves the marker without changing the list, the same split the
    // tray menu makes between rebuilding its actions and moving its tick.
    connect(&controller_, &LightingController::schemeChanged, this,
            &ControlPanel::refreshActiveSchemeMarker);
    connect(&controller_, &LightingController::schemeListChanged, this,
            &ControlPanel::refreshSchemeList);
    connect(&controller_, &LightingController::configErrorChanged, this,
            &ControlPanel::onConfigErrorChanged);

    refreshFromController();
    refreshSchemeList();
    refreshStartupIntegration();
    for (Domain domain : kAllDomains) onStatusChanged(domain, controller_.status(domain));
    onConfigErrorChanged(controller_.configError());
}

// ---------------------------------------------------------------------------- keyboard ---

QWidget* ControlPanel::buildKeyboardTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    keyboardColor_ = new ColorPicker(page);
    layout->addWidget(keyboardColor_);

    auto* form = new QFormLayout;

    keyboardEffect_ = new QComboBox(page);
    // Only effects confirmed on this hardware are offered. Note what is absent and always
    // will be: there is no "off" entry here. Effect type 0 is the APIv5 clear command that
    // leaves the keyboard dark in a state nothing but a reboot recovers, and the type these
    // entries carry cannot represent it. Turning the lights off is a colour, not an effect.
    for (KeyboardEffect effect : kAllKeyboardEffects) {
        const QString label = QString::fromUtf8(toString(effect).data(),
                                                static_cast<int>(toString(effect).size()));
        keyboardEffect_->addItem(label.at(0).toUpper() + label.mid(1),
                                 QVariant::fromValue(static_cast<int>(effect)));
    }
    keyboardEffect_->setEnabled(keyboardEffect_->count() > 1);
    form->addRow(tr("Effect"), keyboardEffect_);

    auto* brightnessRow = new QHBoxLayout;
    keyboardBrightness_ = new QSlider(Qt::Horizontal, page);
    keyboardBrightness_->setRange(1, 100);
    keyboardBrightnessValue_ = new QLabel(page);
    keyboardBrightnessValue_->setMinimumWidth(40);
    brightnessRow->addWidget(keyboardBrightness_, 1);
    brightnessRow->addWidget(keyboardBrightnessValue_);
    form->addRow(tr("Brightness"), brightnessRow);

    layout->addLayout(form);
    layout->addStretch(1);

    connect(keyboardColor_, &ColorPicker::colorChanged, this, [this](const QColor& color) {
        if (loading_) return;
        working_.keyboard.color = toRgb(color);
        pushScheme();
    });
    connect(keyboardColor_, &ColorPicker::colorCommitted, this,
            [this] { controller_.flushPendingSave(); });
    connect(keyboardBrightness_, &QSlider::valueChanged, this, [this](int percent) {
        keyboardBrightnessValue_->setText(QStringLiteral("%1%").arg(percent));
        if (loading_) return;
        working_.keyboard.brightness = Brightness::fromPercent(percent);
        pushScheme();
    });
    connect(keyboardEffect_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        working_.keyboard.effect =
            static_cast<KeyboardEffect>(keyboardEffect_->itemData(index).toInt());
        pushScheme();
    });

    return page;
}

// ---------------------------------------------------------------------------- external ---

QWidget* ControlPanel::buildExternalTab() {
    auto* page = new QWidget(this);
    auto* layout = new QHBoxLayout(page);

    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel(tr("Zones"), page));
    zoneList_ = new QListWidget(page);
    zoneList_->setToolTip(
        tr("Double-click to rename a zone once you know which light it drives."));
    left->addWidget(zoneList_, 1);

    applyToAllZones_ = new QPushButton(tr("Apply colour to all zones"), page);
    left->addWidget(applyToAllZones_);

    auto* brightnessRow = new QHBoxLayout;
    externalBrightness_ = new QSlider(Qt::Horizontal, page);
    externalBrightness_->setRange(1, 100);
    externalBrightnessValue_ = new QLabel(page);
    externalBrightnessValue_->setMinimumWidth(40);
    brightnessRow->addWidget(new QLabel(tr("Brightness"), page));
    brightnessRow->addWidget(externalBrightness_, 1);
    brightnessRow->addWidget(externalBrightnessValue_);
    left->addLayout(brightnessRow);

    auto* right = new QVBoxLayout;
    zoneColor_ = new ColorPicker(page);
    right->addWidget(new QLabel(tr("On AC power"), page));
    right->addWidget(zoneColor_);

    zoneSeparateBattery_ = new QCheckBox(tr("Different colour on battery"), page);
    zoneSeparateBattery_->setToolTip(
        tr("These are power-type lights: the controller stores an AC colour and a battery "
           "colour and switches between them itself."));
    right->addWidget(zoneSeparateBattery_);

    zoneBatteryColor_ = new ColorPicker(page);
    right->addWidget(zoneBatteryColor_);
    right->addStretch(1);

    layout->addLayout(left, 1);
    layout->addLayout(right, 1);

    connect(zoneList_, &QListWidget::currentRowChanged, this,
            [this](int) { refreshZoneEditor(); });
    connect(zoneList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (loading_ || !item) return;
        const int index = zoneList_->row(item);
        if (index < 0 || index >= static_cast<int>(working_.external.zones.size())) return;
        working_.external.zones[static_cast<std::size_t>(index)].name =
            item->text().toStdString();
        pushScheme();
    });
    connect(zoneColor_, &ColorPicker::colorChanged, this, [this](const QColor& color) {
        if (loading_) return;
        const int index = selectedZoneIndex();
        if (index < 0) return;
        working_.external.zones[static_cast<std::size_t>(index)].color.ac = toRgb(color);
        pushScheme();
    });
    connect(zoneBatteryColor_, &ColorPicker::colorChanged, this, [this](const QColor& color) {
        if (loading_) return;
        const int index = selectedZoneIndex();
        if (index < 0) return;
        working_.external.zones[static_cast<std::size_t>(index)].color.battery = toRgb(color);
        pushScheme();
    });
    for (ColorPicker* picker : {zoneColor_, zoneBatteryColor_})
        connect(picker, &ColorPicker::colorCommitted, this,
                [this] { controller_.flushPendingSave(); });
    connect(zoneSeparateBattery_, &QCheckBox::toggled, this, [this](bool separate) {
        zoneBatteryColor_->setEnabled(separate);
        if (loading_) return;
        const int index = selectedZoneIndex();
        if (index < 0) return;
        ColorPair& pair = working_.external.zones[static_cast<std::size_t>(index)].color;
        pair.separateBatteryColor = separate;
        if (!separate) pair.battery = pair.ac;
        pushScheme();
    });
    connect(applyToAllZones_, &QPushButton::clicked, this, [this] {
        const int index = selectedZoneIndex();
        if (index < 0) return;
        const ColorPair source = working_.external.zones[static_cast<std::size_t>(index)].color;
        for (ExternalZone& zone : working_.external.zones) zone.color = source;
        pushScheme();
        refreshZoneEditor();
    });
    connect(externalBrightness_, &QSlider::valueChanged, this, [this](int percent) {
        externalBrightnessValue_->setText(QStringLiteral("%1%").arg(percent));
        if (loading_) return;
        working_.external.brightness = Brightness::fromPercent(percent);
        pushScheme();
    });

    return page;
}

// ----------------------------------------------------------------------------- schemes ---

QWidget* ControlPanel::buildSchemesTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    schemeList_ = new QListWidget(page);
    layout->addWidget(schemeList_, 1);

    auto* buttons = new QHBoxLayout;
    auto* activate = new QPushButton(tr("Activate"), page);
    auto* saveAs = new QPushButton(tr("Save current as..."), page);
    removeScheme_ = new QPushButton(tr("Delete"), page);
    buttons->addWidget(activate);
    buttons->addWidget(saveAs);
    buttons->addWidget(removeScheme_);
    layout->addLayout(buttons);

    auto* startup = new QGroupBox(tr("Apply at login and after resume"), page);
    auto* startupLayout = new QVBoxLayout(startup);
    startupStatus_ = new QLabel(startup);
    startupStatus_->setWordWrap(true);
    startupToggle_ = new QPushButton(startup);
    startupLayout->addWidget(startupStatus_);
    startupLayout->addWidget(startupToggle_);
    layout->addWidget(startup);

    connect(activate, &QPushButton::clicked, this, [this] {
        const QString name = schemeNameOf(schemeList_->currentItem());
        if (!name.isEmpty()) controller_.activateScheme(name);
    });
    connect(schemeList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const QString name = schemeNameOf(item);
        if (!name.isEmpty()) controller_.activateScheme(name);
    });
    connect(saveAs, &QPushButton::clicked, this, [this] {
        bool accepted = false;
        const QString name =
            QInputDialog::getText(this, tr("Save scheme"), tr("Name"), QLineEdit::Normal,
                                  QString::fromStdString(working_.name), &accepted);
        if (accepted && !name.trimmed().isEmpty())
            controller_.saveWorkingSchemeAs(name.trimmed());
    });
    connect(removeScheme_, &QPushButton::clicked, this, [this] {
        const QString name = schemeNameOf(schemeList_->currentItem());
        if (name.isEmpty()) return;
        const auto answer = QMessageBox::question(
            this, tr("Delete scheme"), tr("Delete the scheme \"%1\"?").arg(name));
        if (answer == QMessageBox::Yes) controller_.deleteScheme(name);
    });
    connect(startupToggle_, &QPushButton::clicked, this, [this] {
        const IntegrationStatus status = checkStartupIntegration();
        std::string error;
        const bool ok = status.isManaged() ? removeStartupIntegration(error)
                                           : installStartupIntegration(error);
        if (!ok)
            QMessageBox::warning(this, tr("Apply at login"),
                                 QString::fromStdString(error));
        refreshStartupIntegration();
    });

    return page;
}

// ------------------------------------------------------------------------------ state ---

int ControlPanel::selectedZoneIndex() const {
    if (!zoneList_) return -1;
    const int row = zoneList_->currentRow();
    if (row < 0 || row >= static_cast<int>(working_.external.zones.size())) return -1;
    return row;
}

void ControlPanel::pushScheme() {
    if (loading_) return;
    controller_.applyWorkingScheme(working_);
}

void ControlPanel::refreshFromController() {
    const Scheme incoming = controller_.workingScheme();
    const bool sameZones = incoming.external.zones.size() == working_.external.zones.size();
    working_ = incoming;

    {
        LoadingScope loading(loading_);
        keyboardColor_->setColor(toQColor(working_.keyboard.color));
        keyboardBrightness_->setValue(working_.keyboard.brightness.percent());
        keyboardBrightnessValue_->setText(
            QStringLiteral("%1%").arg(working_.keyboard.brightness.percent()));
        for (int i = 0; i < keyboardEffect_->count(); ++i) {
            if (keyboardEffect_->itemData(i).toInt() ==
                static_cast<int>(working_.keyboard.effect)) {
                keyboardEffect_->setCurrentIndex(i);
                break;
            }
        }

        externalBrightness_->setValue(working_.external.brightness.percent());
        externalBrightnessValue_->setText(
            QStringLiteral("%1%").arg(working_.external.brightness.percent()));

        const int previousRow = zoneList_->currentRow();
        zoneList_->clear();
        for (const ExternalZone& zone : working_.external.zones) {
            auto* item = new QListWidgetItem(QString::fromStdString(zone.name), zoneList_);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
        if (zoneList_->count() > 0)
            zoneList_->setCurrentRow(sameZones && previousRow >= 0 ? previousRow : 0);
    }

    refreshZoneEditor();
    refreshDeleteButton();
}

void ControlPanel::refreshZoneEditor() {
    const int index = selectedZoneIndex();
    const bool valid = index >= 0;

    zoneColor_->setEnabled(valid);
    zoneSeparateBattery_->setEnabled(valid);

    LoadingScope loading(loading_);
    if (valid) {
        const ColorPair& pair = working_.external.zones[static_cast<std::size_t>(index)].color;
        zoneColor_->setColor(toQColor(pair.ac));
        zoneBatteryColor_->setColor(toQColor(pair.effectiveBattery()));
        zoneSeparateBattery_->setChecked(pair.separateBatteryColor);
        zoneBatteryColor_->setEnabled(pair.separateBatteryColor);
    } else {
        zoneBatteryColor_->setEnabled(false);
    }
}

void ControlPanel::refreshSchemeList() {
    if (!schemeList_) return;
    const QString previous = schemeNameOf(schemeList_->currentItem());
    const QString active = QString::fromStdString(controller_.config().activeSchemeName);

    schemeList_->clear();
    QListWidgetItem* activeItem = nullptr;
    for (const Scheme& scheme : controller_.config().schemes) {
        const QString name = QString::fromStdString(scheme.name);
        auto* item = new QListWidgetItem(name, schemeList_);
        item->setData(Qt::UserRole, name);
        if (name == previous) schemeList_->setCurrentItem(item);
        if (name == active) activeItem = item;
    }

    // A rebuild always has to leave something current. Deleting the selected scheme removes
    // the name this was restoring, and Activate and Delete both act on the selection - so
    // without a fallback they stay enabled and silently do nothing when pressed.
    if (!schemeList_->currentItem()) {
        if (activeItem)
            schemeList_->setCurrentItem(activeItem);
        else if (schemeList_->count() > 0)
            schemeList_->setCurrentRow(0);
    }

    refreshActiveSchemeMarker();
    refreshDeleteButton();
}

// LightingController::deleteScheme refuses to remove the last scheme, and it has to stay
// that way - but the user must never confirm a prompt for something that then does nothing.
void ControlPanel::refreshDeleteButton() {
    if (!removeScheme_) return;
    const bool canDelete = controller_.config().schemes.size() > 1;

    removeScheme_->setEnabled(canDelete);
    removeScheme_->setToolTip(canDelete ? tr("Delete the selected scheme.")
                                        : tr("The last scheme cannot be deleted - there "
                                             "would be nothing left to apply."));
}

void ControlPanel::refreshActiveSchemeMarker() {
    if (!schemeList_) return;
    const QString active = QString::fromStdString(controller_.config().activeSchemeName);

    for (int row = 0; row < schemeList_->count(); ++row) {
        QListWidgetItem* item = schemeList_->item(row);
        const QString name = schemeNameOf(item);
        const bool isActive = name == active;

        QFont font = item->font();
        font.setBold(isActive);
        item->setFont(font);
        item->setText(isActive ? tr("%1  (active)").arg(name) : name);
    }
}

void ControlPanel::refreshStartupIntegration() {
    const IntegrationStatus status = checkStartupIntegration();
    startupStatus_->setText(QString::fromStdString(status.summary()));
    startupToggle_->setText(status.isManaged() ? tr("Disable") : tr("Enable"));
    startupToggle_->setToolTip(
        tr("Rewrites %1 so the existing login unit and resume hook apply your active "
           "scheme. Nothing else is installed or enabled.")
            .arg(QString::fromStdString(status.shimPath.string())));
}

void ControlPanel::onConfigErrorChanged(const QString& error) {
    if (!configStatus_) return;
    configStatus_->setText(error.isEmpty() ? QString()
                                           : tr("Configuration problem: %1").arg(error));
    configStatus_->setVisible(!error.isEmpty());
}

void ControlPanel::onStatusChanged(Domain domain, DomainStatus status) {
    QLabel* label = domain == Domain::Keyboard ? keyboardStatus_ : externalStatus_;
    if (!label) return;
    label->setText(QString::fromStdString(describe(domain, status)));
    label->setStyleSheet(statusStyle(status));
}

}  // namespace alienfx::ui
