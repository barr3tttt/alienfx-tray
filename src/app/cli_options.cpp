#include "app/cli_options.h"

#include <charconv>
#include <utility>

namespace alienfx {
namespace {

// Qt's documented standard options, accepted in both spellings. The ones marked as taking
// a value take it as a separate token - `-style Fusion` - which must not then be mistaken
// for an option of its own and rejected.
struct StandardOption {
    std::string_view name;
    bool takesValue;
};

constexpr StandardOption kQtStandardOptions[] = {
    {"style", true},           {"stylesheet", true},      {"platform", true},
    {"platformpluginpath", true}, {"platformtheme", true}, {"plugin", true},
    {"session", true},         {"widgetcount", false},    {"reverse", false},
    {"qmljsdebugger", true},   {"qwindowgeometry", true}, {"qwindowicon", true},
    {"qwindowtitle", true},    {"display", true},         {"geometry", true},
    {"title", true},           {"name", true},
};

const StandardOption* findStandardOption(std::string_view name) {
    for (const StandardOption& option : kQtStandardOptions)
        if (option.name == name) return &option;
    return nullptr;
}

// The argument with one or two leading dashes removed, or nothing when it is not
// dash-prefixed at all. Exactly two, so `---style` stays unrecognised.
std::optional<std::string_view> optionName(std::string_view argument) {
    if (argument.starts_with("--")) return argument.substr(2);
    if (argument.starts_with("-")) return argument.substr(1);
    return std::nullopt;
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

CliRequest rejected(std::string message) {
    return CliRequest{CliMode::Error, std::nullopt, std::move(message)};
}

}  // namespace

CliRequest parseCommandLine(const std::vector<std::string_view>& arguments) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];

        if (argument == "--help" || argument == "-h") return {CliMode::Help, std::nullopt, {}};
        if (argument == "--version") return {CliMode::Version, std::nullopt, {}};
        if (argument == "--apply") return {CliMode::Apply, std::nullopt, {}};
        if (argument == "--off") return {CliMode::Off, std::nullopt, {}};
        if (argument == "--probe-external") return {CliMode::ProbeExternal, std::nullopt, {}};
        if (argument.starts_with("--probe-external=")) {
            const std::optional<int> id = parseTrailingId(argument);
            if (!id) return rejected("expected --probe-external=<number>");
            return {CliMode::ProbeExternal, id, {}};
        }

        const std::optional<std::string_view> name = optionName(argument);
        // A bare token belongs to QApplication, or to the option before it.
        if (!name) continue;

        const StandardOption* standard = findStandardOption(name->substr(0, name->find('=')));
        if (!standard) return rejected("unknown option " + std::string(argument));

        // `-style=Fusion` carries its value inline; `-style Fusion` does not.
        if (standard->takesValue && name->find('=') == std::string_view::npos) ++i;
    }
    return {};
}

}  // namespace alienfx
