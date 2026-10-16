#pragma once
#include <QString>

namespace Theme
{
// Stable settings values: "system", "light", "dark". Unknown values follow the system.
void initialize(const QString &mode);
void apply(const QString &mode);
QString mode();
bool isDark();
}
