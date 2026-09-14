#pragma once

#include <QColor>

#include "core/color.h"

namespace alienfx::ui {

[[nodiscard]] inline QColor toQColor(Rgb color) { return QColor(color.r, color.g, color.b); }

[[nodiscard]] inline Rgb toRgb(const QColor& color) {
    return Rgb{static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
               static_cast<std::uint8_t>(color.blue())};
}

}  // namespace alienfx::ui
