#include <QApplication>
#include <QMessageBox>

#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/apply_mode.h"
#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "app/signal_handler.h"
#include "app/single_instance.h"
#include "ui/tray_icon.h"

namespace {

constexpr const char* kVersion = "0.1.0";

void printUsage(std::ostream& out) {
    out << "alienfx-tray " << kVersion << " - RGB lighting for Alienware laptops on Linux\n"
        << "\n"
        << "Usage:\n"
        << "  alienfx-tray                    run the tray icon\n"
        << "  alienfx-tray --apply            apply the saved scheme and exit\n"
        << "  alienfx-tray --off              black out every light and exit\n"
        << "  alienfx-tray --probe-external   identify the external lights by colour\n"
        << "  alienfx-tray --probe-external=N identify one external light on its own\n"
        << "  alienfx-tray --version\n"
        << "  alienfx-tray --help\n"
        << "\n"
        << "--apply is what the login unit and the resume hook run. It needs no display.\n";
}

std::optional<int> parseTrailingId(std::string_view argument) {
    const std::size_t equals = argument.find('=');
    if (equals == std::string_view::npos) return std::nullopt;

    const std::string_view digits = argument.substr(equals + 1);
    int value = 0;
    const auto* begin = digits.data();
    const auto [ptr, ec] = std::from_chars(begin, begin + digits.size(), value);
    if (ec != std::errc{} || ptr != begin + digits.size()) return std::nullopt;
    return value;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);

    // Headless modes are handled before any QApplication exists. The resume hook runs from a
    // root sleep script with no DISPLAY, no WAYLAND_DISPLAY and no session bus, so --apply
    // must not need a GUI toolkit connection to work.
    for (std::string_view argument : arguments) {
        if (argument == "--help" || argument == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (argument == "--version") {
            std::cout << kVersion << "\n";
            return 0;
        }
        if (argument == "--apply") return alienfx::runApplyMode(std::cout, std::cerr);
        if (argument == "--off") return alienfx::runOffMode(std::cout, std::cerr);
        if (argument == "--probe-external")
            return alienfx::runProbeExternalMode(std::nullopt, std::cout, std::cerr);
        if (argument.starts_with("--probe-external=")) {
            const std::optional<int> id = parseTrailingId(argument);
            if (!id) {
                std::cerr << "alienfx-tray: expected --probe-external=<number>\n";
                return 2;
            }
            return alienfx::runProbeExternalMode(id, std::cout, std::cerr);
        }
        // Anything else belongs to QApplication. Qt's own standard options are real - an
        // X11 session manager restores this process with `-session <id>` - and a tray that
        // refuses to start on session restore is one nobody can rely on.
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("alienfx-tray"));
    QApplication::setApplicationVersion(QString::fromLatin1(kVersion));
    QApplication::setDesktopFileName(QStringLiteral("alienfx-tray"));
    // The window closing is not the application quitting: this lives in the tray.
    QApplication::setQuitOnLastWindowClosed(false);

    alienfx::registerMetaTypes();

    alienfx::SingleInstance instance;
    if (!instance.claim()) {
        if (alienfx::SingleInstance::signalExistingInstance()) return 0;
        std::cerr << "alienfx-tray: another instance is running but did not respond\n";
        return 1;
    }

    alienfx::LightingController controller;
    alienfx::ui::TrayIcon tray(controller);

    QObject::connect(&instance, &alienfx::SingleInstance::raiseRequested, &tray,
                     &alienfx::ui::TrayIcon::togglePanel);

    // SIGTERM from a session logout has to reach the event loop: the graceful shutdown is
    // what releases the claim on the keyboard interface.
    auto* signalHandler = new alienfx::UnixSignalHandler(&app);
    if (signalHandler->install()) {
        QObject::connect(signalHandler, &alienfx::UnixSignalHandler::terminationRequested, &app,
                         &QApplication::quit);
    }

    // Release the devices before Qt tears anything down, whatever route we take out.
    QObject::connect(&app, &QApplication::aboutToQuit, &controller,
                     &alienfx::LightingController::shutdown);

    if (!tray.isSystemTrayAvailable()) {
        // Plasma always has one, but a bare session or a crashed panel might not. Say so
        // rather than running invisibly with no way to reach the panel.
        QMessageBox::warning(
            nullptr, QApplication::translate("main", "alienfx-tray"),
            QApplication::translate(
                "main", "No system tray is available in this session. The control panel is "
                        "open, but there will be no tray icon."));
        tray.togglePanel();
    } else {
        tray.show();
    }

    return QApplication::exec();
}
