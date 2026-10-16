#pragma once
#include <QFont>
#include <QString>

// Presentation only: never store this result back into a message, quote, or draft.
QString wechatDisplayText(const QString &source);
QFont wechatDisplayFont(const QFont &base);
