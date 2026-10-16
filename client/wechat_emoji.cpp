#include "wechat_emoji.h"
#include <QHash>
#include <QStringList>

namespace
{
const QHash<QString, QString> &aliases()
{
    // Unicode equivalents, rather than copies of WeChat's proprietary image assets.
    static const auto values = []
    {
        QHash<QString, QString> result;
        const struct { const char *names; const char *emoji; } entries[] = {
            {"微笑", "🙂"}, {"撇嘴", "😒"}, {"色", "😍"}, {"发呆|發呆", "😳"},
            {"得意", "😎"}, {"流泪|流淚", "😢"}, {"害羞", "😊"}, {"闭嘴|閉嘴", "🤐"},
            {"睡", "😴"}, {"大哭", "😭"}, {"尴尬|尷尬", "😅"}, {"发怒|發怒", "😠"},
            {"调皮|調皮", "😛"}, {"呲牙", "😁"}, {"惊讶|驚訝", "😮"}, {"难过|難過", "😞"},
            {"酷", "😎"}, {"冷汗", "😰"}, {"抓狂", "😫"}, {"吐", "🤮"},
            {"偷笑", "🤭"}, {"愉快", "😄"}, {"白眼|翻白眼", "🙄"}, {"傲慢", "😏"},
            {"饥饿|飢餓", "🤤"}, {"困", "🥱"}, {"惊恐|驚恐|恐惧|恐懼", "😱"},
            {"流汗|汗|擦汗", "😓"}, {"憨笑", "😆"}, {"悠闲|悠閒", "😌"},
            {"咒骂|咒罵", "🤬"}, {"疑问|疑問", "🤔"}, {"嘘|噓", "🤫"}, {"晕|暈", "😵"},
            {"衰", "😩"}, {"骷髅|骷髏", "💀"}, {"敲打", "🔨"}, {"再见|再見", "👋"},
            {"抠鼻|摳鼻", "👃"}, {"鼓掌", "👏"}, {"糗大了", "😖"}, {"坏笑|壞笑", "😏"},
            {"左哼哼|右哼哼", "😤"}, {"哈欠", "🥱"}, {"鄙视|鄙視", "😒"}, {"委屈", "🥺"},
            {"快哭了", "😥"}, {"阴险|陰險|奸笑", "😈"}, {"亲亲|親親", "😘"},
            {"吓|嚇", "😨"}, {"可怜|可憐", "🥺"}, {"拥抱|擁抱", "🤗"},
            {"笑脸|笑臉", "😃"}, {"生病", "😷"}, {"脸红|臉紅", "😊"},
            {"破涕为笑|破涕為笑", "😂"}, {"失望", "😔"}, {"无语|無語", "😑"},
            {"嘿哈", "😜"}, {"捂脸|捂臉", "🤦"}, {"机智|機智", "😎"},
            {"皱眉|皺眉", "😟"}, {"耶|胜利|勝利", "✌️"}, {"吃瓜", "🍉"},
            {"加油", "💪"}, {"天啊", "😲"}, {"社会社会|社會社會", "🕶️"}, {"旺柴", "🐶"},
            {"好的|OK", "👌"}, {"打脸|打臉", "🤦"}, {"哇", "🤩"}, {"敬礼|敬禮", "🫡"},
            {"让我看看|讓我看看", "👀"}, {"叹气|嘆氣", "😮‍💨"}, {"苦涩|苦澀", "🥲"},
            {"裂开|裂開", "💔"}, {"强|強|666", "👍"}, {"弱|差劲|差勁", "👎"},
            {"握手", "🤝"}, {"抱拳|合十", "🙏"}, {"勾引", "🫴"}, {"拳头|拳頭", "✊"},
            {"爱你|愛你", "🤟"}, {"NO", "🙅"}, {"爱情|愛情", "💑"}, {"飞吻|飛吻", "😘"},
            {"跳跳|跳绳|跳繩", "🤸"}, {"发抖|發抖", "🥶"}, {"怄火|慪火", "😤"},
            {"转圈|轉圈", "💃"}, {"磕头|磕頭", "🙇"}, {"回头|回頭", "🙋"},
            {"鸡|雞", "🐔"}, {"猪头|豬頭", "🐷"}, {"爱心|愛心|心", "❤️"}, {"心碎", "💔"},
            {"玫瑰", "🌹"}, {"凋谢|凋謝", "🥀"}, {"嘴唇", "💋"}, {"咖啡", "☕"},
            {"饭|飯", "🍚"}, {"月亮", "🌙"}, {"太阳|太陽", "☀️"}, {"西瓜", "🍉"},
            {"蛋糕", "🎂"}, {"啤酒", "🍺"}, {"足球", "⚽"}, {"瓢虫|瓢蟲", "🐞"},
            {"炸弹|炸彈", "💣"}, {"菜刀", "🔪"}, {"便便", "💩"}, {"礼物|禮物", "🎁"},
            {"红包|紅包", "🧧"}, {"发|發", "🀅"}, {"庆祝|慶祝", "🎉"},
            {"烟花|煙花", "🎆"}, {"爆竹", "🧨"}, {"福", "🧧"}
        };
        for (const auto &entry : entries)
            for (const auto &name : QString::fromUtf8(entry.names).split('|'))
                result.insert(name, QString::fromUtf8(entry.emoji));
        return result;
    }();
    return values;
}
}

QString wechatDisplayText(const QString &source)
{
    auto opening = source.indexOf('[');
    if (opening < 0)
        return source;
    QString display = source.left(opening);
    display.reserve(source.size());
    while (opening >= 0)
    {
        const auto closing = source.indexOf(']', opening + 1);
        if (closing < 0)
        {
            display += QStringView(source).mid(opening);
            break;
        }
        const auto length = closing - opening - 1;
        const auto name = length <= 16 ? source.mid(opening + 1, length) : QString{};
        const auto value = aliases().constFind(name);
        display += value == aliases().cend() ? source.mid(opening, length + 2) : *value;
        const auto next = source.indexOf('[', closing + 1);
        display += QStringView(source).mid(closing + 1, next < 0 ? -1 : next - closing - 1);
        opening = next;
    }
    return display;
}

QFont wechatDisplayFont(const QFont &base)
{
    auto font = base;
    auto families = font.families();
#if defined(Q_OS_WIN)
    const QStringList fallback{"Segoe UI Emoji", "Segoe UI Symbol"};
#elif defined(Q_OS_MACOS)
    const QStringList fallback{"Apple Color Emoji", "Apple Symbols"};
#else
    const QStringList fallback{"Noto Color Emoji", "Noto Sans Symbols 2"};
#endif
    for (const auto &family : fallback)
        if (!families.contains(family, Qt::CaseInsensitive))
            families.append(family);
    font.setFamilies(families);
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() & ~QFont::NoFontMerging));
    return font;
}
