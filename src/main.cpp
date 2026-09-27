#include <QApplication>
#include <QMessageBox>

#include <iostream>
#include <string_view>
#include <vector>

#include "app/apply_mode.h"
#include "app/cli_options.h"
#include "app/lighting_controller.h"
#include "app/metatypes.h"
#include "app/signal_handler.h"
#include "app/single_instance.h"
#include "ui/tray_icon.h"

namespace {

// The version comes from the CMake project() declaration and the git revision the binary
// was built from, so --version answers "what am I running" in one line. Both are supplied
// by the build, the revision through a header regenerated on every build; the fallbacks
// cover a build outside CMake or outside a checkout.
#if __has_include("git_revision.h")
#include "git_revision.h"
#endif
#ifndef ALIENFX_TRAY_VERSION
#define ALIENFX_TRAY_VERSION "0.0.0"
#endif
#ifndef ALIENFX_TRAY_GIT_SHA
#define ALIENFX_TRAY_GIT_SHA "unknown"
#endif

constexpr const char* kVersion = ALIENFX_TRAY_VERSION;
constexpr const char* kGitSha = ALIENFX_TRAY_GIT_SHA;

void printUsage(std::ostream& out) {
    out << "alienfx-tray " << kVersion << " (" << kGitSha
        << ") - RGB lighting for Alienware laptops on Linux\n"
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

}  // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);

    // Headless modes are handled before any QApplication exists. The resume hook runs from a
    // root sleep script with no DISPLAY, no WAYLAND_DISPLAY and no session bus, so --apply
    // must not need a GUI toolkit connection to work.
    const alienfx::CliRequest request = alienfx::parseCommandLine(arguments);
    switch (request.mode) {
        case alienfx::CliMode::Help:
            printUsage(std::cout);
            return 0;
        case alienfx::CliMode::Version:
            std::cout << kVersion << " (" << kGitSha << ")\n";
            return 0;
        case alienfx::CliMode::Apply:
            return alienfx::runApplyMode(std::cout, std::cerr);
        case alienfx::CliMode::Off:
            return alienfx::runOffMode(std::cout, std::cerr);
        case alienfx::CliMode::ProbeExternal:
            return alienfx::runProbeExternalMode(request.probeLightId, std::cout, std::cerr);
        case alienfx::CliMode::Error:
            std::cerr << "alienfx-tray: " << request.message << "\n\n";
            printUsage(std::cerr);
            return 2;
        case alienfx::CliMode::Tray:
            break;
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
