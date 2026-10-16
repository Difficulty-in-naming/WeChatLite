#include "attachment.h"
#include "composer.h"
#include "messages.h"
#include "recall_drafts.h"
#include "wechat_emoji.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QGlyphRun>
#include <QInputMethodEvent>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMovie>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtTest>
#include <functional>

class ClientTests : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDirectory;
    static QRect opaqueBounds(const QImage &image)
    {
        QRect bounds;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() >= 250)
                    bounds |= QRect(x, y, 1, 1);
        return bounds;
    }
    static QByteArray animatedGif()
    {
        return QByteArray::fromBase64(
            "R0lGODlhyABkAIEAAP8AAAAAAAAAAAAAACH/"
            "C05FVFNDQVBFMi4wAwEAAAAh+"
            "QQACAAAACwAAAAAyABkAAAI5AABCBxIsKDBgwgTKlzIsKHDhxAjSpxIsaLFixgzatzIsaPHjyBDihxJsqTJkyhTqlzJsqXLl"
            "zBjypxJs6bNmzhz6tzJs6fPn0CDCh1KtKjRo0iTKl3KtKnTp1CjSp1KtarVq1izat3KtavXr2DDih1LtqzZs2jTql3Ltq3bt"
            "3Djyp1Lt67du3jz6t3Lt6/"
            "fv4ADCx5MuLDhw4gTK17MuLHjx5AjS55MubLly5gza97MubPnz6BDix5NurTp06hTq17NurXr17Bjy55Nu7bt27hz697Nu7f"
            "v38CDCx9OvLjx42QDAgAh+"
            "QQBCAABACwAAAAAyABkAIEAgAAAAAAAAAAAAAAI5AABCBxIsKDBgwgTKlzIsKHDhxAjSpxIsaLFixgzatzIsaPHjyBDihxJs"
            "qTJkyhTqlzJsqXLlzBjypxJs6bNmzhz6tzJs6fPn0CDCh1KtKjRo0iTKl3KtKnTp1CjSp1KtarVq1izat3KtavXr2DDih1Lt"
            "qzZs2jTql3Ltq3bt3Djyp1Lt67du3jz6t3Lt6/"
            "fv4ADCx5MuLDhw4gTK17MuLHjx5AjS55MubLly5gza97MubPnz6BDix5NurTp06hTq17NurXr17Bjy55Nu7bt27hz697Nu7f"
            "v38CDCx9OvLjx42QDAgA7");
    }
    static QJsonObject gifMessage(int number, const QString &path)
    {
        return {{"id", QString::number(number)},
                {"seq", number},
                {"sender", "GIF test"},
                {"timestamp", 1'700'000'000'000LL},
                {"media", QJsonArray{QJsonObject{
                              {"id", QString::number(number)}, {"kind", "gif"}, {"_local", path}}}}};
    }
  private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("WeChatLiteTests");
        QCoreApplication::setApplicationName("Client-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    }
    void bracketEmojiAliasesAreDisplayOnlyAndUnknownNamesStayIntact()
    {
        QCOMPARE(wechatDisplayText("[發][红包][凋谢][菜刀][炸弹][便便]"), "🀅🧧🥀🔪💣💩");
        QCOMPARE(wechatDisplayText("[发][紅包][凋謝][炸彈]"), "🀅🧧🥀💣");
        QCOMPARE(wechatDisplayText("[微笑][流泪][流淚][捂脸][捂臉][强][強][爱心][愛心]"),
                 "🙂😢😢🤦🤦👍👍❤️❤️");
        const QString unknown = "[自定义表情] [not-an-emoji] [微笑但未闭合\n[] [nested[便便] 🧧";
        QCOMPARE(wechatDisplayText(unknown), unknown);
        QCOMPARE(wechatDisplayText("前[微笑]中[自定义表情]后[炸弹]"), "前🙂中[自定义表情]后💣");
        const QString ordinary = "普通文字和 Unicode 🙂 保持不变";
        QCOMPARE(wechatDisplayText(ordinary), ordinary);
        QFont base("sans-serif");
        base.setPixelSize(17);
        const auto displayFont = wechatDisplayFont(base);
        QCOMPARE(displayFont.pixelSize(), 17);
        QCOMPARE(displayFont.families().first(), base.families().first());
        QVERIFY(displayFont.families().size() > base.families().size());
        QCOMPARE(wechatDisplayFont(displayFont), displayFont);
    }
    void bracketEmojiMeasurementPaintingCopyAndQuoteAgree()
    {
        const QString source = "[發][红包][凋谢][菜刀][炸弹][便便]\n[微笑][未知表情]";
        const QString visible = "🀅🧧🥀🔪💣💩\n🙂[未知表情]";
        QJsonObject message{{"id", "emoji"}, {"sender", "离线测试"}, {"text", source},
                            {"quote", QJsonObject{{"id", "source"}, {"sender", "成员"}, {"text", "[红包]原文"}}},
                            {"timestamp", 1'700'000'000'000LL}};
        MessageModel model;
        model.replace({message});
        QFont font = QApplication::font();
        font.setPixelSize(16);
        auto converted = message;
        converted["text"] = visible;
        QCOMPARE(MessageDelegate::textHeight(message, 105, font),
                 MessageDelegate::textHeight(converted, 105, font));
        MessageDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 420, 300);
        option.font = font;
        auto render = [&]
        {
            QImage result(option.rect.size(), QImage::Format_RGB32);
            result.fill(Qt::white);
            QPainter painter(&result);
            delegate.paint(&painter, option, model.index(0));
            return result;
        };
        const auto originalImage = render();
        const auto originalBubble = MessageDelegate::bubbleRect(option, model.index(0));
        model.replace({converted});
        QCOMPARE(MessageDelegate::bubbleRect(option, model.index(0)), originalBubble);
        QCOMPARE(render(), originalImage);
        model.replace({message});
        MessageView view;
        view.setModel(&model);
        QSignalSpy quote(&view, &MessageView::quoteRequested);
        auto *menu = view.contextMenuFor(model.index(0), false);
        QVERIFY(menu);
        auto *copy = menu->findChild<QAction *>("copyMessage");
        auto *cite = menu->findChild<QAction *>("quoteMessage");
        QVERIFY(copy && cite);
        copy->trigger();
        QCOMPARE(QApplication::clipboard()->text(), source);
        cite->trigger();
        QCOMPARE(quote.count(), 1);
        QCOMPARE(quote.first().first().toJsonObject()["text"].toString(), source);
        QCOMPARE(quote.first().first().toJsonObject()["quote"].toObject(), message["quote"].toObject());
        QCOMPARE(model.rows.first()["text"].toString(), source);
        QCOMPARE(messageQuoteLabel(message["quote"].toObject()), "成员: 🧧原文");
        delete menu;
    }
    void requestedEmojiHaveRealGlyphsAndRenderInTheNativeMessageView()
    {
        const QStringList emoji{"🀅", "🧧", "🥀", "🔪", "💣", "💩"};
        auto font = wechatDisplayFont(QApplication::font());
        font.setPixelSize(22);
        for (const auto &symbol : emoji)
        {
            QTextLayout layout(symbol, font);
            layout.beginLayout();
            auto line = layout.createLine();
            line.setLineWidth(100);
            layout.endLayout();
            const auto runs = layout.glyphRuns();
            QVERIFY2(!runs.isEmpty(), qPrintable(symbol));
            int count = 0;
            for (const auto &run : runs)
                for (const auto index : run.glyphIndexes())
                {
                    QVERIFY2(index != 0, qPrintable("missing glyph: " + symbol));
                    ++count;
                }
            QVERIFY(count > 0);
        }
        MessageModel model;
        model.replace({QJsonObject{{"id", "emoji-shot"}, {"sender", "离线表情测试"},
                                   {"text", "[發][红包][凋谢][菜刀][炸弹][便便]"},
                                   {"timestamp", 1'700'000'000'000LL}},
                       QJsonObject{{"id", "emoji-reply"}, {"outgoing", true},
                                   {"text", "简体 [微笑][捂脸][强]，繁體 [流淚][紅包][凋謝]"},
                                   {"quote", QJsonObject{{"id", "emoji-shot"}, {"sender", "离线表情测试"},
                                                          {"text", "[發][红包][凋谢][菜刀][炸弹][便便]"}}},
                                   {"timestamp", 1'700'000'000'001LL}}});
        MessageView view;
        view.resize(650, 330);
        view.setFont(font);
        view.setModel(&model);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(30);
        const auto image = view.grab();
        QVERIFY(!image.isNull());
        const auto directory = qEnvironmentVariable("WECHAT_LITE_EMOJI_SHOT_DIR");
        if (!directory.isEmpty())
        {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(image.save(QDir(directory).filePath("wechat-emoji.png")));
        }
    }
    void messageAndCachedMenuColorsFollowThePaletteWithoutChangingLayout()
    {
        struct RestorePalette
        {
            QPalette original = qApp->palette();
            ~RestorePalette() { qApp->setPalette(original); }
        } restore;
        auto selectDark = [&](bool dark)
        {
            auto palette = restore.original;
            palette.setColor(QPalette::Window, QColor(dark ? "#202020" : "#fafafa"));
            qApp->setPalette(palette);
        };
        selectDark(false);
        MessageModel model;
        model.replace({QJsonObject{{"id", "theme"}, {"sender", "离线成员"}, {"text", "正文 [红包]"},
                                   {"outgoing", true}, {"quote", QJsonObject{{"id", "source"},
                                                                           {"sender", "成员"}, {"text", "引用"}}}}});
        MessageDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 520, 180);
        option.font = QApplication::font();
        auto render = [&]
        {
            QImage image(option.rect.size(), QImage::Format_RGB32);
            image.fill(qApp->palette().color(QPalette::Window));
            QPainter painter(&image);
            delegate.paint(&painter, option, model.index(0));
            return image;
        };
        const auto bubble = MessageDelegate::bubbleRect(option, model.index(0));
        const auto quote = MessageDelegate::quoteRect(option, model.index(0));
        const auto sample = bubble.bottomRight() - QPoint(8, 5);
        const auto lightImage = render();
        QCOMPARE(lightImage.pixelColor(sample), QColor("#9df29f"));
        const QJsonObject member{{"id", "offline"}, {"name", "成员"}};
        const auto lightAvatar = chatAvatar(member, 36).toImage();
        MessageView view;
        view.setModel(&model);
        auto *menu = view.contextMenuFor(model.index(0), false);
        QVERIFY(menu);
        menu->popup(QPoint(40, 40));
        menu->setActiveAction(nullptr);
        QTest::qWait(10);
        auto panelPixel = [menu]
        {
            const auto image = menu->grab().toImage();
            const auto ratio = menu->devicePixelRatioF();
            return image.pixelColor(qRound(menu->width() / 2.0 * ratio), qRound(27 * ratio));
        };
        QCOMPARE(panelPixel(), QColor("#f7f7f7"));
        selectDark(true);
        const auto darkImage = render();
        QCOMPARE(darkImage.pixelColor(sample), QColor("#3c633c"));
        QCOMPARE(MessageDelegate::bubbleRect(option, model.index(0)), bubble);
        QCOMPARE(MessageDelegate::quoteRect(option, model.index(0)), quote);
        QCOMPARE(darkImage.pixelColor(quote.right() - 1, quote.center().y()), QColor("#51565a"));
        QVERIFY(chatAvatar(member, 36).toImage() != lightAvatar);
        QCOMPARE(panelPixel(), QColor("#303234")); // The same popup must invalidate its light background cache.
        selectDark(false);
        QCOMPARE(panelPixel(), QColor("#f7f7f7"));
        QCOMPARE(chatAvatar(member, 36).toImage(), lightAvatar);
        QCOMPARE(render(), lightImage);
        delete menu;
    }
    void delayedMediaLayoutsKeepTheReadingAnchor_data()
    {
        QTest::addColumn<QString>("position");
        QTest::newRow("middle") << QString("middle");
        QTest::newRow("near-bottom") << QString("near-bottom");
        QTest::newRow("bottom") << QString("bottom");
    }
    void delayedMediaLayoutsKeepTheReadingAnchor()
    {
        QFETCH(QString, position);
        QTemporaryDir directory;
        const auto wide = directory.filePath("delayed-wide.png");
        const auto tall = directory.filePath("delayed-tall.png");
        QImage image(240, 35, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(wide));
        image = QImage(35, 240, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(tall));
        QFile attachment(directory.filePath("delayed-file.txt"));
        QVERIFY(attachment.open(QIODevice::WriteOnly));
        attachment.write("offline attachment");
        attachment.close();
        QJsonArray history;
        for (int i = 0; i < 45; ++i)
        {
            QJsonObject message{{"id", QString::number(i)}, {"seq", i + 1},
                                {"timestamp", 1'700'000'000'000LL}, {"text", "reading position"}};
            if (i == 3 || i == 8 || i == 9)
                message["media"] = QJsonArray{QJsonObject{{"id", "media-" + QString::number(i)},
                                                        {"kind", i == 9 ? "file" : "image"}}};
            history.append(message);
        }
        MessageModel model;
        model.replace(history);
        MessageView view;
        view.resize(620, 320);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        if (position == "middle")
        {
            view.scrollTo(model.index(20), QAbstractItemView::PositionAtTop);
            bar->setValue(bar->value() + 17);
        }
        else
            bar->setValue(bar->maximum() - (position == "near-bottom" ? 7 : 0));
        QTest::qWait(20);
        const QPersistentModelIndex anchor(view.indexAt(QPoint(1, 0)));
        QVERIFY(anchor.isValid());
        const auto offset = view.visualRect(anchor).top();
        const auto initialMaximum = bar->maximum();
        QStringList jumps;
        class PaintWitness final : public QObject
        {
          public:
            std::function<void()> observe;
            bool eventFilter(QObject *object, QEvent *event) override
            {
                if (event->type() == QEvent::Paint)
                    observe();
                return QObject::eventFilter(object, event);
            }
        } witness;
        witness.observe = [&]
        {
            const auto actual = view.visualRect(anchor).top();
            if ((position == "bottom" && bar->value() != bar->maximum()) ||
                (position != "bottom" && actual != offset))
                jumps.append(QString("offset=%1 expected=%2 value=%3 maximum=%4")
                                 .arg(actual).arg(offset).arg(bar->value()).arg(bar->maximum()));
        };
        view.viewport()->installEventFilter(&witness);
        QTimer::singleShot(15, &model, [&] { model.setLocalMedia("media-3", wide); });
        QTimer::singleShot(50, &model, [&] { model.setLocalMedia("media-8", tall); });
        QTimer::singleShot(85, &model, [&] { model.setLocalMedia("media-9", attachment.fileName()); });
        QTest::qWait(140);
        QVERIFY(bar->maximum() != initialMaximum); // Real headers changed row heights; no placeholder-only test.
        model.upsert({{"id", "new-message"}, {"seq", 100}, {"timestamp", 1'700'000'000'100LL},
                      {"text", "new message while media arrives"}});
        QTest::qWait(30);
        QVERIFY2(jumps.isEmpty(), qPrintable(jumps.join(';')));
        if (position == "bottom")
            QCOMPARE(bar->value(), bar->maximum());
        else
        {
            QCOMPARE(view.visualRect(anchor).top(), offset);
            QVERIFY(bar->value() < bar->maximum());
        }
    }
    void mediaLayoutKeepsScrollBarVisibilityAndSubsequentPixelScrolling()
    {
        QTemporaryDir directory;
        const auto tall = directory.filePath("scrollbar-tall.png");
        const auto wide = directory.filePath("scrollbar-wide.png");
        QImage image(35, 240, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor(y, 80, 170));
        QVERIFY(image.save(tall));
        image = QImage(240, 35, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(wide));
        MessageModel model;
        model.replace({QJsonObject{{"id", "only"}, {"timestamp", 1'700'000'000'000LL},
                                   {"media", QJsonArray{QJsonObject{{"id", "resized"}, {"kind", "image"}}}}}});
        class ScrollWitness final : public MessageView
        {
          public:
            QList<int> deltas;
          protected:
            void scrollContentsBy(int dx, int dy) override
            {
                deltas.append(dy);
                MessageView::scrollContentsBy(dx, dy);
            }
        } view;
        view.resize(620, 250);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        QTRY_COMPARE(bar->maximum(), 0);
        QVERIFY(!bar->isVisible());
        model.setLocalMedia("resized", tall);
        QTRY_VERIFY(bar->maximum() > 1);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), 500);
        QCOMPARE(bar->value(), bar->maximum());
        QTest::qWait(20);
        const auto before = view.viewport()->grab().toImage();
        const auto previous = bar->value();
        view.deltas.clear();
        bar->setValue(previous - 1);
        QTest::qWait(20);
        QCOMPARE(bar->value(), previous - 1);
        QCOMPARE(view.deltas, QList<int>{1});
        const auto after = view.viewport()->grab().toImage();
        // A one-pixel upward scroll moves the actual painted image down by exactly one pixel.
        const auto scale = view.devicePixelRatioF();
        const QPoint sample(qRound(85 * scale), qRound(110 * scale));
        QCOMPARE(after.pixelColor(sample + QPoint(0, qRound(scale))), before.pixelColor(sample));
        model.setLocalMedia("resized", wide);
        QTRY_COMPARE(bar->maximum(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 500);
        QCOMPARE(bar->value(), 0);
    }
    void mediaLayoutMovesTheWheelAnimationWithItsReadingAnchor()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("animated-layout.png");
        QImage image(35, 240, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(path));
        MessageModel model;
        QJsonArray history;
        for (int i = 0; i < 70; ++i)
        {
            QJsonObject message{{"id", QString::number(i)}, {"seq", i + 1}, {"text", "history"}};
            if (i == 3)
                message["media"] = QJsonArray{QJsonObject{{"id", "wheel-image"}, {"kind", "image"}}};
            history.append(message);
        }
        model.replace(history);
        MessageView view;
        view.resize(620, 320);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        view.scrollTo(model.index(30), QAbstractItemView::PositionAtTop);
        auto *animation = view.findChild<QVariantAnimation *>("smoothScrollAnimation");
        QVERIFY(animation);
        QWheelEvent wheel(QPointF(20, 20), view.viewport()->mapToGlobal(QPoint(20, 20)), {},
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view.viewport(), &wheel);
        animation->setCurrentTime(40);
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        const auto start = animation->startValue().toReal(), target = animation->endValue().toReal();
        const auto originalValue = bar->value(), originalMaximum = bar->maximum();
        const QPersistentModelIndex anchor(view.indexAt(QPoint(1, 0)));
        const auto offset = view.visualRect(anchor).top();
        model.setLocalMedia("wheel-image", path);
        view.doItemsLayout();
        const auto documentShift = bar->maximum() - originalMaximum;
        QVERIFY(documentShift > 0);
        QCOMPARE(bar->value(), originalValue + documentShift);
        QCOMPARE(view.visualRect(anchor).top(), offset);
        QCOMPARE(animation->currentTime(), 40);
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        QCOMPARE(animation->endValue().toReal(), target + documentShift);
        animation->setCurrentTime(80);
        QCOMPARE(bar->value(), qRound(start + documentShift + (target - start) * .875));
        QVERIFY(bar->value() < originalValue + documentShift);
        bar->setValue(bar->value() + 23);
        const auto manualValue = bar->value();
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
        QTest::qWait(180);
        QCOMPARE(bar->value(), manualValue);
        const auto wide = directory.filePath("second-delayed-image.png");
        image = QImage(240, 35, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(wide));
        const QPersistentModelIndex manualAnchor(view.indexAt(QPoint(1, 0)));
        const auto manualOffset = view.visualRect(manualAnchor).top();
        model.setLocalMedia("wheel-image", wide);
        bar->setValue(bar->value() - 37); // A gesture between dataChanged and Qt's queued layout wins.
        view.doItemsLayout();
        QCOMPARE(view.visualRect(manualAnchor).top(), manualOffset + 37);
    }
    void upwardGestureBeforeQueuedLayoutDoesNotFollowNewMessagesToTheBottom()
    {
        MessageModel model;
        QJsonArray history;
        for (int i = 0; i < 60; ++i)
            history.append(QJsonObject{{"id", QString::number(i)}, {"seq", i + 1}, {"text", "history"}});
        model.replace(history);
        MessageView view;
        view.resize(620, 320);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        view.scrollToBottom();
        const auto original = bar->value();
        model.upsert({{"id", "new"}, {"seq", 100}, {"text", QString("new message ").repeated(40)}});
        QWheelEvent wheel(QPointF(20, 20), view.viewport()->mapToGlobal(QPoint(20, 20)), {},
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view.viewport(), &wheel);
        view.doItemsLayout();
        QCOMPARE(bar->value(), original);
        QVERIFY(bar->value() < bar->maximum());
        QVERIFY(view.scrollAnimationActive());
    }
    void prependingHistoryKeepsTheAnchorAtTheTimeOfTheModelUpdate()
    {
        MessageModel model;
        QJsonArray history;
        for (int i = 0; i < 70; ++i)
            history.append(QJsonObject{{"id", QString::number(i)}, {"seq", i + 1},
                                       {"timestamp", 1'700'000'000'000LL + i}, {"text", "history"}});
        model.replace(history);
        MessageView view;
        view.resize(620, 320);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        QTRY_VERIFY(bar->maximum() > 2000);
        view.scrollTo(model.index(30), QAbstractItemView::PositionAtTop);
        bar->setValue(bar->value() + 13);
        const auto anchor = view.indexAt(QPoint(1, 0)).data(Qt::UserRole).toJsonObject()["id"].toString();
        const auto offset = view.visualRect(view.indexAt(QPoint(1, 0))).top();
        model.prepend({QJsonObject{{"id", "older"}, {"timestamp", 1'600'000'000'000LL},
                                  {"text", QString("older content ").repeated(60)}}});
        view.doItemsLayout();
        const auto current = view.indexAt(QPoint(1, 0));
        QCOMPARE(current.data(Qt::UserRole).toJsonObject()["id"].toString(), anchor);
        QCOMPARE(view.visualRect(current).top(), offset);
    }
    void wheelScrollingEasesAccumulatesAndRespectsManualMovement()
    {
        MessageModel model;
        QJsonArray history;
        for (int i = 0; i < 100; ++i)
            history.append(QJsonObject{{"id", QString::number(i)}, {"seq", i + 1}, {"text", "history"}});
        model.replace(history);
        MessageView view;
        view.resize(500, 250);
        view.setModel(&model);
        view.show();
        auto *bar = view.verticalScrollBar();
        auto *animation = view.findChild<QVariantAnimation *>("smoothScrollAnimation");
        QVERIFY(animation);
        QTRY_VERIFY(bar->maximum() > 1500);
        bar->setValue(600);
        auto wheel = [&view](int angle, QPoint pixels = {})
        {
            QWheelEvent event(QPointF(20, 20), view.viewport()->mapToGlobal(QPoint(20, 20)), pixels,
                              QPoint(0, angle), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(view.viewport(), &event);
        };
        wheel(-120);
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        const auto firstTarget = animation->endValue().toReal();
        QVERIFY(firstTarget > 600);
        animation->setCurrentTime(80);
        QVERIFY(bar->value() > 600 && bar->value() < firstTarget);
        QVERIFY(qAbs(bar->value() - (600 + (firstTarget - 600) * .875)) <= 1);
        wheel(-120);
        QVERIFY(animation->endValue().toReal() > firstTarget);
        animation->setCurrentTime(40);
        const auto turn = bar->value();
        wheel(120);
        QVERIFY(animation->endValue().toReal() < turn);
        const auto upwardTarget = animation->endValue();
        model.upsert({{"id", "appended"}, {"seq", 101}, {"timestamp", 100}, {"text", "new message"}});
        view.doItemsLayout();
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        QCOMPARE(animation->endValue(), upwardTarget);
        bar->setValue(900); // A scrollbar drag / external positioning takes precedence.
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
        QTest::qWait(180);
        QCOMPARE(bar->value(), 900);
        wheel(-120);
        QCOMPARE(animation->state(), QAbstractAnimation::Running);
        wheel(0, QPoint(0, -23)); // Keep native touchpad pixel scrolling and its inertia.
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
        wheel(-120);
        model.prepend({QJsonObject{{"id", "older"}, {"seq", -1}, {"text", "older"}}});
        QCOMPARE(animation->state(), QAbstractAnimation::Stopped);
        view.setProperty("locatedHistory", true);
        QSignalSpy latest(&view, &MessageView::latestRequested);
        QTest::keyClick(&view, Qt::Key_End);
        QCOMPARE(latest.count(), 1);
    }
    void quoteIsIndependentBelowReplyAndItsMenuOnlyLocates()
    {
        MessageModel model;
        const QJsonObject quote{{"id", "original"}, {"sender", "成员"}, {"text", "原文内容"}};
        QJsonObject reply{
            {"id", "reply"}, {"group", "A"}, {"text", "好"}, {"outgoing", true}, {"quote", quote}};
        model.replace({reply});
        MessageView view;
        view.setModel(&model);
        MessageDelegate delegate;
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 640, 200);
        option.font.setPixelSize(14);
        auto render = [&]
        {
            QImage image(option.rect.size(), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            delegate.paint(&painter, option, model.index(0));
            return image;
        };
        const auto quoted = render();
        const auto reference = MessageDelegate::quoteRect(option, model.index(0));
        QVERIFY(!reference.isEmpty());
        QCOMPARE(quoted.pixelColor(reference.right() - 1, reference.center().y()), QColor("#e2e2e2"));
        QCOMPARE(quoted.pixelColor(reference.left() + 1, reference.center().y()), QColor(Qt::white));
        int greenBottom = 0, greenCount = 0;
        for (int y = 0; y < quoted.height(); ++y)
            for (int x = 0; x < quoted.width(); ++x)
                if (quoted.pixelColor(x, y) == QColor("#9df29f"))
                {
                    greenBottom = y;
                    ++greenCount;
                }
        QVERIFY(greenCount > 0 && reference.top() > greenBottom + 3);
        reply.remove("quote");
        model.replace({reply});
        const auto plain = render();
        int plainGreenCount = 0;
        for (int y = 0; y < plain.height(); ++y)
            for (int x = 0; x < plain.width(); ++x)
                plainGreenCount += plain.pixelColor(x, y) == QColor("#9df29f");
        QCOMPARE(greenCount, plainGreenCount); // Referencing a long source never enlarges the reply bubble.
        reply["quote"] = quote;
        model.replace({reply});
        QSignalSpy locate(&view, &MessageView::locateRequested);
        auto *menu = view.quoteContextMenuFor(model.index(0));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 1);
        QCOMPARE(menu->actions().first()->text(), "定位到原文位置");
        menu->actions().first()->trigger();
        QCOMPARE(locate.count(), 1);
        QCOMPARE(locate.first().first().toJsonObject()["id"].toString(), "original");
        QCOMPARE(locate.first().first().toJsonObject()["group"].toString(), "A");
        delete menu;
        reply["quote"] = QJsonObject{{"id", "original"}, {"unavailable", true}};
        model.replace({reply});
        menu = view.quoteContextMenuFor(model.index(0));
        QVERIFY(menu && !menu->actions().first()->isEnabled());
        delete menu;
    }
    void recallDraftCacheKeepsLocalCopyAndHonorsScopeAndExpiry()
    {
        RecallDrafts drafts;
        const auto identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
        drafts.configure(identity);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        AttachmentStore attachments;
        QMimeData image;
        QImage pixels(8, 8, QImage::Format_RGB32);
        pixels.fill(Qt::red);
        image.setImageData(pixels);
        QVERIFY(attachments.addImage(&image));
        const auto original = attachments.items.first().path;
        const QJsonObject quote{{"id", "source"}, {"sender", "成员"}, {"text", "quoted"}};
        drafts.remember("outgoing", "A",
                        {{"text", "draft"}, {"quote", quote}, {"attachments", attachments.serialize()}}, now);
        auto cached = drafts.get("outgoing", "A", now + 1);
        QCOMPARE(cached["text"].toString(), "draft");
        QCOMPARE(cached["quote"].toObject(), quote);
        const auto backup = cached["attachments"].toArray().first().toObject()["path"].toString();
        QVERIFY(backup != original && QFile::exists(backup));
        attachments.clear();
        QVERIFY(!QFile::exists(original));
        QCOMPARE(drafts.get("outgoing", "A", now + 86399999)["attachments"].toArray().size(), 1);
        QVERIFY(drafts.get("outgoing", "A", now + 86400000).isEmpty());
        QVERIFY(drafts.get("outgoing", "A", now - 1).isEmpty());
        QVERIFY(drafts.get("outgoing", "B", now).isEmpty());
        RecallDrafts other;
        other.configure(identity + "other-client");
        QVERIFY(other.get("outgoing", "A", now).isEmpty());
        QVERIFY(drafts.release("outgoing", cached));
        const auto released = cached["attachments"].toArray().first().toObject()["path"].toString();
        QVERIFY(released != backup && QFile::exists(released));
        QVERIFY(!QFile::exists(backup));
        drafts.consume("outgoing");
        QVERIFY(drafts.get("outgoing", "A", now).isEmpty());
        QVERIFY(QFile::exists(released)); // Restored composer owns the file until it sends/discards it.
        AttachmentStore restored;
        restored.restore(cached["attachments"].toArray());
        restored.clear();
        QVERIFY(!QFile::exists(released));
    }
    void recallDraftCacheCapsRecordsAndOmitsOversizedTemporaryCopies()
    {
        RecallDrafts drafts;
        drafts.configure(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < 65; ++i)
            drafts.remember(QString::number(i), "A", {{"text", "kept"}}, now);
        QVERIFY(drafts.get("0", "A", now).isEmpty());
        QCOMPARE(drafts.get("64", "A", now)["text"].toString(), "kept");
        QTemporaryDir files;
        QFile oversized(files.filePath("large-video.mp4"));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(128LL * 1024 * 1024 + 1));
        oversized.close();
        const QFileInfo info(oversized);
        drafts.remember(
            "large", "A",
            {{"text", "keep this text"},
             {"attachments", QJsonArray{QJsonObject{{"path", info.absoluteFilePath()},
                                                    {"size", info.size()},
                                                    {"modified", info.lastModified().toMSecsSinceEpoch()},
                                                    {"temporary", true}}}}},
            now);
        const auto retained = drafts.get("large", "A", now);
        QCOMPARE(retained["text"].toString(), "keep this text");
        QVERIFY(retained["attachments"].toArray().isEmpty());
        QCOMPARE(retained["_missingAttachments"].toInt(), 1);
        QVERIFY(QFile::exists(info.absoluteFilePath())); // A cache limit never deletes a source attachment.
        for (int i = 0; i < 65; ++i)
            drafts.consume(QString::number(i));
        drafts.consume("large");
    }
    void filePasteAttachesInsteadOfInsertingPath()
    {
        Composer composer;
        QSignalSpy spy(&composer, &Composer::filesAttached);
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile("/tmp/clip.mp4"), QUrl::fromLocalFile("/tmp/photo.gif")});
        QApplication::clipboard()->setMimeData(mime);
        composer.paste();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList().size(), 2);
        QVERIFY(composer.toPlainText().isEmpty());
    }
    void dragDropAttachesToDraft()
    {
        Composer composer;
        composer.resize(400, 150);
        composer.show();
        QSignalSpy spy(&composer, &Composer::filesAttached);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile("/tmp/report.pdf")});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(composer.viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(composer.viewport(), &drop);
        QCOMPARE(spy.count(), 1);
        QVERIFY(composer.toPlainText().isEmpty());
    }
    void pastedImageSurvivesUntilDiscard()
    {
        AttachmentStore store;
        Composer composer;
        connect(&composer, &Composer::imageAttached, &store,
                [&](const QMimeData *mime) { store.addImage(mime); });
        auto *mime = new QMimeData;
        QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(Qt::red);
        mime->setImageData(image);
        QApplication::clipboard()->setMimeData(mime);
        composer.paste();
        QCOMPARE(store.items.size(), 1);
        QVERIFY(store.items.first().temporary);
        auto path = store.items.first().path;
        QVERIFY(QFile::exists(path));
        QCOMPARE(store.items.first().kind, "image");
        store.remove(0);
        QVERIFY(!QFile::exists(path));
    }
    void gifPastePreservesOriginalBytes()
    {
        QMimeData mime;
        auto bytes = QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
        mime.setData("image/gif", bytes);
        AttachmentStore store;
        QVERIFY(store.addImage(&mime));
        QFile file(store.items.first().path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
        QCOMPARE(store.items.first().kind, "gif");
        file.close();
        store.clear();
    }
    void changedFilesCannotBeSilentlySent()
    {
        QTemporaryDir directory;
        auto path = directory.filePath("video.mp4");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("0000ftyp");
        file.close();
        AttachmentStore store;
        store.addFiles({path, path, directory.path()});
        QCOMPARE(store.items.size(), 1);
        QCOMPARE(store.items.first().kind, "video");
        QVERIFY(store.validate().isEmpty());
        QVERIFY(file.open(QIODevice::Append));
        file.write("changed");
        file.close();
        QVERIFY(!store.validate().isEmpty());
    }
    void attachmentsCannotChangeDuringSend()
    {
        QTemporaryDir directory;
        auto path = directory.filePath("report.txt");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("data");
        file.close();
        AttachmentStore store;
        store.addFiles({path});
        store.setLocked(true);
        store.remove(0);
        QCOMPARE(store.items.size(), 1);
        store.addFiles({path});
        QCOMPARE(store.items.size(), 1);
        store.setLocked(false);
        store.remove(0);
        QVERIFY(store.items.isEmpty());
    }
    void restoringDraftDoesNotMentionPlainTextNames()
    {
        Composer composer;
        QString text = "@同名 普通文字 @同名";
        composer.restore(
            text, {QJsonObject{{"id", "wxid_selected"}, {"name", "同名"}, {"_start", 0}, {"_length", 3}}});
        auto mentions = composer.mentions();
        QCOMPARE(mentions.size(), 1);
        QCOMPARE(mentions.first()["id"].toString(), "wxid_selected");
        QCOMPARE(mentions.first()["_start"].toInt(), 0);
    }
    void atPopupProducesActualMemberId()
    {
        Composer composer;
        composer.resize(400, 150);
        composer.setMembers({QJsonObject{{"id", "wxid_unique"}, {"name", "林小满"}, {"remark", "设计"}}});
        composer.show();
        composer.activateWindow();
        composer.setFocus();
        QTest::qWait(30);
        QTest::keyClicks(&composer, "@");
        QTRY_VERIFY(composer.popupVisible());
        QTest::keyClick(&composer, Qt::Key_Return);
        QCOMPARE(composer.mentions().size(), 1);
        QCOMPARE(composer.mentions().first()["id"].toString(), "wxid_unique");
        QVERIFY(composer.toPlainText().startsWith("@林小满"));
        QTest::keyClick(&composer, Qt::Key_Backspace);
        QTest::keyClick(&composer, Qt::Key_Backspace);
        QVERIFY(composer.mentions().isEmpty());
    }
    void enterRespectsChineseInputComposition()
    {
        Composer composer;
        composer.show();
        QSignalSpy spy(&composer, &Composer::sendRequested);
        QInputMethodEvent preedit("ni", {});
        QApplication::sendEvent(&composer, &preedit);
        QTest::keyClick(&composer, Qt::Key_Return);
        QCOMPARE(spy.count(), 0);
        QInputMethodEvent commit;
        commit.setCommitString("你");
        QApplication::sendEvent(&composer, &commit);
        QTest::keyClick(&composer, Qt::Key_Return);
        QCOMPARE(spy.count(), 1);
        QTest::keyClick(&composer, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(spy.count(), 1);
    }
    void avatarMentionInsertsTheSelectedIdentityAtTheDraftCursor()
    {
        Composer composer;
        composer.setPlainText("draft tail");
        auto cursor = composer.textCursor();
        cursor.setPosition(5);
        composer.setTextCursor(cursor);
        composer.insertMention({{"id", "wxid_right_person"}, {"name", "同名"}});
        QCOMPARE(composer.toPlainText(), QString("draft\u2005@同名\u2005 tail"));
        QCOMPARE(composer.mentions().size(), 1);
        QCOMPARE(composer.mentions().first()["id"].toString(), "wxid_right_person");
        Composer restored;
        restored.restore(composer.toPlainText(), composer.mentions());
        QCOMPARE(restored.mentions(), composer.mentions());
        composer.setReadOnly(true);
        composer.insertMention({{"id", "other"}, {"name", "同名"}});
        QCOMPARE(composer.mentions().size(), 1);
    }
    void recallMenuRequiresARecentConfirmedOwnMessage()
    {
        const qint64 now = 1'800'000'000'000LL;
        QJsonObject message{{"id", "outgoing"},
                            {"outgoing", true},
                            {"status", "sent"},
                            {"source_id", "confirmed"},
                            {"timestamp", now - 180000}};
        QVERIFY(!canRecallMessage(message, now));
        message["timestamp"] = now - 179999;
        QVERIFY(canRecallMessage(message, now));
        message["timestamp"] = now - 180001;
        QVERIFY(!canRecallMessage(message, now));
        message["timestamp"] = now + 1;
        QVERIFY(!canRecallMessage(message, now));
        message["timestamp"] = now;
        for (const auto status : {"queued", "sending", "failed", "uncertain", "recalled"})
        {
            message["status"] = status;
            QVERIFY(!canRecallMessage(message, now));
        }
        message["status"] = "received";
        for (const auto state : {"pending", "uncertain"})
        {
            message["recall_status"] = state;
            QVERIFY(!canRecallMessage(message, now));
        }
        message["recall_status"] = "failed";
        QVERIFY(!canRecallMessage(message, now));
        message["recall_status"] = "";
        message["source_id"] = "";
        QVERIFY(!canRecallMessage(message, now));
        message["source_id"] = "confirmed";
        message["outgoing"] = false;
        QVERIFY(!canRecallMessage(message, now));
    }
    void contextMenusCopyQuoteMentionAndUseReferenceHighlight()
    {
        MessageModel model;
        model.replace({QJsonObject{{"id", "menu"},
                                   {"sender_id", "wxid_member"},
                                   {"sender", "成员"},
                                   {"text", "完整文字\n第二行"},
                                   {"outgoing", true},
                                   {"status", "sent"},
                                   {"source_id", "wx-source"},
                                   {"timestamp", QDateTime::currentMSecsSinceEpoch()}}});
        MessageView view;
        view.setModel(&model);
        QSignalSpy mention(&view, &MessageView::mentionRequested), quote(&view, &MessageView::quoteRequested),
            recall(&view, &MessageView::recallRequested);
        auto *avatar = view.contextMenuFor(model.index(0), true);
        QVERIFY(avatar);
        QCOMPARE(avatar->actions().size(), 1);
        avatar->actions().first()->trigger();
        QCOMPARE(mention.first().first().toJsonObject()["id"].toString(), "wxid_member");
        delete avatar;
        auto *menu = view.contextMenuFor(model.index(0), false);
        QVERIFY(menu);
        auto *copy = menu->findChild<QAction *>("copyMessage");
        auto *cite = menu->findChild<QAction *>("quoteMessage");
        auto *undo = menu->findChild<QAction *>("recallMessage");
        QVERIFY(copy && cite && undo);
        QCOMPARE(menu->actions().size(), 4);
        QCOMPARE(menu->actions()[0], copy);
        QCOMPARE(menu->actions()[1], cite);
        QVERIFY(menu->actions()[2]->isSeparator());
        QCOMPARE(menu->actions()[3], undo);
        copy->trigger();
        QCOMPARE(QApplication::clipboard()->text(), "完整文字\n第二行");
        cite->trigger();
        undo->trigger();
        QCOMPARE(quote.first().first().toJsonObject()["id"].toString(), "menu");
        QCOMPARE(recall.count(), 1);
        menu->popup(QPoint(20, 20));
        menu->setActiveAction(copy);
        QTest::qWait(10);
        const auto image = menu->grab().toImage();
        const auto rect = menu->actionGeometry(copy);
        const auto ratio = menu->devicePixelRatioF();
        QCOMPARE(image.pixelColor(qRound((rect.left() + 3) * ratio), qRound(rect.center().y() * ratio)),
                 QColor("#00c777"));
        delete menu;
    }
    void menusHaveARealTransparentSoftShadowAndLightBorder()
    {
        MessageModel model;
        model.replace({QJsonObject{{"id", "shadow"}, {"sender", "测试成员"}, {"text", "右键菜单样式"}}});
        MessageView view;
        view.resize(520, 340);
        view.setStyleSheet("background:#fafafa; border:0;");
        view.setWindowFlag(Qt::WindowStaysOnTopHint);
        view.setModel(&model);
        view.move(view.screen()->availableGeometry().topLeft() + QPoint(60, 60));
        view.show();
        if (QGuiApplication::platformName() == "windows")
        {
            view.raise();
            view.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&view));
        }
        auto *menu = view.contextMenuFor(model.index(0), false);
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 2);
        QVERIFY(menu->windowFlags().testFlag(Qt::Popup));
        QVERIFY(menu->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(menu->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        QVERIFY(menu->testAttribute(Qt::WA_TranslucentBackground));
        menu->popup(view.mapToGlobal(QPoint(130, 100)));
        QTest::qWait(QGuiApplication::platformName() == "windows" ? 300 : 20);
        menu->setActiveAction(nullptr);
        const auto image = menu->grab().toImage();
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(image.width() - 1, image.height() - 1).alpha(), 0);
        const auto body = opaqueBounds(image);
        const auto ratio = menu->devicePixelRatioF();
        QVERIFY(body.left() >= qRound(20 * ratio));
        QVERIFY(qAbs(body.width() / ratio - 162) <= 2);
        const auto nearShadow = image.pixelColor(body.left() - qRound(3 * ratio), body.center().y());
        const auto farShadow = image.pixelColor(body.left() - qRound(13 * ratio), body.center().y());
        QVERIFY(nearShadow.alpha() > 10 && nearShadow.alpha() < 70);
        QVERIFY(farShadow.alpha() > 0 && farShadow.alpha() < nearShadow.alpha());
        const auto border = image.pixelColor(body.left(), body.center().y());
        QVERIFY(border.red() >= 200 && border.red() < 235 && border.red() == border.green());
        QCOMPARE(image.pixelColor(body.left() + qRound(4 * ratio), body.center().y()), QColor("#f7f7f7"));
        const auto capture = qEnvironmentVariable("WECHAT_LITE_MENU_CAPTURE_DIR");
        if (!capture.isEmpty())
        {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(menu->grab().save(QDir(capture).filePath("menu-alpha.png")));
            if (QGuiApplication::platformName() == "windows")
            {
                // Layered popup HWNDs have no readable window DC. Crop the composited desktop using
                // the actual image/screen ratio, including a QT_SCALE_FACTOR different from OS DPI.
                auto *screen = menu->screen();
                const auto desktop = screen->grabWindow(0).toImage();
                const auto screenRect = screen->geometry();
                const auto frame = menu->frameGeometry();
                const qreal scaleX = qreal(desktop.width()) / screenRect.width();
                const qreal scaleY = qreal(desktop.height()) / screenRect.height();
                const QRect crop(qRound((frame.left() - screenRect.left()) * scaleX),
                                 qRound((frame.top() - screenRect.top()) * scaleY),
                                 qRound(frame.width() * scaleX), qRound(frame.height() * scaleY));
                const auto native = desktop.copy(crop);
                QVERIFY(!native.isNull());
                const auto paper = native.pixelColor(qRound((body.left() / ratio + 4) * scaleX),
                                                     qRound(body.center().y() / ratio * scaleY));
                QVERIFY2(qAbs(paper.red() - 247) <= 2 && qAbs(paper.green() - 247) <= 2,
                         qPrintable(paper.name()));
                QVERIFY(native.save(QDir(capture).filePath("menu-native.png")));
            }
            menu->setActiveAction(menu->actions().first());
            QVERIFY(menu->grab().save(QDir(capture).filePath("menu-hover-alpha.png")));
        }
        delete menu;
    }
    void menuPanelStaysAtTheSourceAndTheWholeShadowStaysOnScreen()
    {
        MessageModel model;
        model.replace({QJsonObject{{"id", "anchor"}, {"text", "定位菜单"}}});
        MessageView view;
        view.resize(520, 320);
        view.move(100, 100);
        view.setModel(&model);
        view.show();
        QTest::qWait(10);
        auto popupFor =
            [&](const QPoint &click, QContextMenuEvent::Reason reason, QPoint *panelPosition, QRect *frame)
        {
            bool seen = false;
            QTimer::singleShot(0, &view,
                               [&]
                               {
                                   auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                                   if (!menu)
                                       return;
                                   const auto bounds = opaqueBounds(menu->grab().toImage());
                                   const auto ratio = menu->devicePixelRatioF();
                                   *panelPosition = menu->pos() + QPoint(qRound(bounds.left() / ratio),
                                                                         qRound(bounds.top() / ratio));
                                   *frame = menu->frameGeometry();
                                   seen = true;
                                   menu->close();
                               });
            // Local QWidget coordinates are authoritative; a stale native/global point must not offset DPI
            // placement.
            QContextMenuEvent context(reason, click, QPoint(1, 1));
            QApplication::sendEvent(view.viewport(), &context);
            QCoreApplication::processEvents();
            return seen;
        };
        QStyleOptionViewItem option;
        option.rect = view.visualRect(model.index(0));
        option.font = view.font();
        const auto bubble = MessageDelegate::bubbleRect(option, model.index(0));
        const auto click = bubble.bottomRight() - QPoint(8, 8);
        QPoint panel;
        QRect frame;
        QVERIFY(popupFor(click, QContextMenuEvent::Mouse, &panel, &frame));
        QVERIFY((panel - view.viewport()->mapToGlobal(click)).manhattanLength() <= 2);
        view.setCurrentIndex(model.index(0));
        QVERIFY(popupFor(QPoint(-1, -1), QContextMenuEvent::Keyboard, &panel, &frame));
        QVERIFY(
            (panel - view.viewport()->mapToGlobal(bubble.bottomRight() + QPoint(2, 2))).manhattanLength() <=
            2);
        QJsonArray messages;
        for (int i = 0; i < 20; ++i)
            messages.append(QJsonObject{
                {"id", QString::number(i)}, {"seq", i}, {"text", "屏幕边缘"}, {"outgoing", true}});
        model.replace(messages);
        const auto available = view.screen()->availableGeometry();
        view.move(available.right() - view.width() - 2, available.bottom() - view.height() - 2);
        view.scrollToBottom();
        QTest::qWait(10);
        option.rect = view.visualRect(model.index(model.rowCount() - 1));
        const auto lastBubble = MessageDelegate::bubbleRect(option, model.index(model.rowCount() - 1));
        QVERIFY(popupFor(lastBubble.bottomRight() - QPoint(8, 8), QContextMenuEvent::Mouse, &panel, &frame));
        QVERIFY(available.contains(frame));
    }
    void recalledMessagesClearMediaAndInvalidateStructuredReferences()
    {
        MessageModel model;
        model.replace(
            {QJsonObject{{"id", "source"}, {"text", "secret"}, {"timestamp", 1}},
             QJsonObject{{"id", "reply"},
                         {"timestamp", 2},
                         {"text", "reply"},
                         {"quote", QJsonObject{{"id", "source"}, {"sender", "成员"}, {"text", "secret"}}}}});
        model.upsert({{"id", "source"},
                      {"timestamp", 1},
                      {"status", "recalled"},
                      {"text", "secret"},
                      {"media", QJsonArray{QJsonObject{{"id", "attachment"}}}}});
        QVERIFY(model.rows[0]["text"].toString().isEmpty());
        QVERIFY(model.rows[0]["media"].toArray().isEmpty());
        const auto quote = model.rows[1]["quote"].toObject();
        QVERIFY(quote["unavailable"].toBool());
        QVERIFY(!messageQuoteLabel(quote).contains("secret"));
        QCOMPARE(model.data(model.index(0), Qt::AccessibleTextRole).toString(), "消息已撤回");
        QVERIFY(!model.data(model.index(0), Qt::ToolTipRole).isValid());
        QVERIFY(!model.data(model.index(1), Qt::ToolTipRole).isValid());
        MessageView view;
        view.setModel(&model);
        QVERIFY(!view.contextMenuFor(model.index(0), false));
        model.setCanReedit("source", true);
        model.upsert({{"id", "source"}, {"timestamp", 1}, {"status", "sent"}, {"text", "stale HTTP body"}});
        QVERIFY(model.rows[0]["text"].toString().isEmpty());
        QVERIFY(model.rows[0]["_can_reedit"].toBool());
        model.replace({QJsonObject{{"id", "reply"},
                                   {"timestamp", 2},
                                   {"text", "reply"},
                                   {"quote", QJsonObject{{"id", "source"}, {"text", "stale quote"}}}},
                       QJsonObject{{"id", "source"}, {"timestamp", 1}, {"text", "stale history body"}}});
        QVERIFY(model.rows[0]["text"].toString().isEmpty());
        QVERIFY(model.rows[1]["quote"].toObject()["unavailable"].toBool());
    }
    void textPasteRemainsPlainText()
    {
        Composer composer;
        QApplication::clipboard()->setText("你好\nworld");
        composer.paste();
        QCOMPARE(composer.toPlainText(), QString("你好\nworld"));
    }
    void longChatTextWrapsInsteadOfClipping()
    {
        QFont font;
        font.setPixelSize(14);
        QJsonObject single{{"text", "hello"}},
            longText{{"text", QString("这是完整的长消息，用于验证聊天气泡换行。").repeated(30)}},
            url{{"text", QString("https://example.com/") + QString("very-long-path-").repeated(40)}};
        auto one = MessageDelegate::textHeight(single, 160, font);
        QVERIFY(MessageDelegate::textHeight(longText, 160, font) > one);
        QVERIFY(MessageDelegate::textHeight(url, 160, font) > one);
        QJsonObject lines{{"text", "first\nsecond\nthird"}};
        QVERIFY(MessageDelegate::textHeight(lines, 160, font) > one);
    }
    void messageWindowStaysBoundedAndUpdatesInPlace()
    {
        MessageModel model;
        for (int i = 1; i <= 900; ++i)
            model.upsert({{"id", QString::number(i)}, {"seq", i}, {"text", "history"}, {"status", "queued"}});
        QCOMPARE(model.rowCount(), 300);
        model.upsert({{"id", "900"}, {"seq", 900}, {"text", "history"}, {"status", "sent"}});
        QCOMPARE(model.rowCount(), 300);
        QCOMPARE(model.rows.last()["status"].toString(), "sent");
        model.upsert({{"id", "old"}, {"seq", 1}, {"text", "late old event"}});
        QCOMPARE(model.rowCount(), 300);
        QCOMPARE(model.rows.last()["id"].toString(), "900");
    }
    void visibleGifsAnimateAtReferenceSizeAndShareOneDecoder()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("animated.gif");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(animatedGif());
        file.close();
        MessageModel model;
        model.replace({gifMessage(1, path), gifMessage(2, path)});
        MessageView view;
        view.resize(500, 400);
        view.setModel(&model);
        view.show();
        QTRY_COMPARE(view.findChildren<QMovie *>().size(), 1);
        auto *movie = view.findChild<QMovie *>();
        QCOMPARE(movie->cacheMode(), QMovie::CacheNone);
        QCOMPARE(movie->scaledSize(), QSize(134, 67));
        QSignalSpy frames(movie, &QMovie::frameChanged);
        QTRY_VERIFY(frames.count() >= 3);
        QSet<int> frameNumbers;
        for (const auto &frame : frames)
            frameNumbers.insert(frame.first().toInt());
        QCOMPARE(frameNumbers.size(), 2);
        auto greenFrameIsPainted = [&view]
        {
            const auto image = view.viewport()->grab().toImage();
            for (int y = 0; y < image.height(); y += 4)
                for (int x = 0; x < image.width(); x += 4)
                {
                    const auto pixel = image.pixelColor(x, y);
                    if (pixel.green() > 90 && pixel.red() < 40 && pixel.blue() < 40)
                        return true;
                }
            return false;
        };
        QTRY_VERIFY(greenFrameIsPainted());
        QPointer<QMovie> oldMovie = movie;
        view.hide();
        QTRY_VERIFY(oldMovie.isNull());
        QVERIFY(view.findChildren<QMovie *>().isEmpty());
        view.show();
        QTRY_COMPARE(view.findChildren<QMovie *>().size(), 1);
        model.replace({});
        QTRY_VERIFY(view.findChildren<QMovie *>().isEmpty());
    }
    void visibleGifDecodersStayBoundedAndFollowModelChanges()
    {
        QTemporaryDir directory;
        QJsonArray messages;
        QStringList paths;
        for (int i = 0; i < 8; ++i)
        {
            const auto path = directory.filePath(QString::number(i) + ".gif");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(animatedGif());
            file.close();
            paths.append(path);
            messages.append(gifMessage(i, path));
        }
        MessageModel model;
        model.replace(messages);
        MessageView view;
        view.resize(500, 1400);
        view.setModel(&model);
        view.show();
        QTRY_COMPARE(view.findChildren<QMovie *>().size(), 4);
        QSet<QString> running;
        for (auto *movie : view.findChildren<QMovie *>())
        {
            running.insert(movie->fileName());
            QVERIFY(movie->scaledSize().width() <= 134);
            QVERIFY(movie->scaledSize().height() <= 134);
        }
        for (int i = 0; i < 4; ++i)
            QVERIFY(running.contains(paths[i]));
        MessageModel replacement;
        view.setModel(&replacement);
        QTRY_VERIFY(view.findChildren<QMovie *>().isEmpty());
        model.upsert(gifMessage(20, paths[0]));
        QTest::qWait(20);
        QVERIFY(view.findChildren<QMovie *>().isEmpty());
        replacement.upsert(gifMessage(1, {}));
        QTest::qWait(20);
        QVERIFY(view.findChildren<QMovie *>().isEmpty());
        replacement.setLocalMedia("1", paths[0]);
        QTRY_COMPARE(view.findChildren<QMovie *>().size(), 1);
    }
    void scrollingAndMinimizingReleaseGifDecoders()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("scroll.gif");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(animatedGif());
        file.close();
        MessageModel model;
        QJsonArray messages{gifMessage(1, path)};
        for (int i = 2; i < 25; ++i)
            messages.append(QJsonObject{{"id", QString::number(i)},
                                        {"seq", i},
                                        {"text", "history"},
                                        {"timestamp", 1'700'000'000'000LL + i}});
        model.replace(messages);
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *view = new MessageView;
        layout->addWidget(view);
        view->setModel(&model);
        window.resize(500, 260);
        window.show();
        QTRY_COMPARE(view->findChildren<QMovie *>().size(), 1);
        view->scrollToBottom();
        QTRY_VERIFY(view->findChildren<QMovie *>().isEmpty());
        view->scrollToTop();
        QTRY_COMPARE(view->findChildren<QMovie *>().size(), 1);
        window.showMinimized();
        QTRY_VERIFY(view->findChildren<QMovie *>().isEmpty());
        window.showNormal();
        QTRY_COMPARE(view->findChildren<QMovie *>().size(), 1);
    }
    void lateHistorySortsByMessageTimeAndKeepsLatestAtBottom()
    {
        MessageModel model;
        model.replace({QJsonObject{{"id", "new"}, {"seq", 1}, {"timestamp", 10000}},
                       QJsonObject{{"id", "old"}, {"seq", 2}, {"timestamp", 1000}}});
        QCOMPARE(model.rows.last()["id"].toString(), "new");
        model.upsert({{"id", "backfill"}, {"seq", 3}, {"timestamp", 2000}});
        QCOMPARE(model.rows[1]["id"].toString(), "backfill");
        QCOMPARE(model.rows.last()["id"].toString(), "new");
        model.prepend({QJsonObject{{"id", "oldest"}, {"seq", 30}, {"timestamp", 100}},
                       QJsonObject{{"id", "tie"}, {"seq", 4}, {"timestamp", 2000}}});
        QCOMPARE(model.rows.first()["id"].toString(), "oldest");
        QCOMPARE(model.rows.last()["id"].toString(), "new");
        QCOMPARE(model.rows[3]["id"].toString(), "tie");
        model.upsert({{"id", "old"}, {"seq", 2}, {"timestamp", 12000}});
        QCOMPARE(model.rows.last()["id"].toString(), "old");
        for (int i = 0; i < 400; ++i)
            model.upsert({{"id", QString::number(i)}, {"seq", i + 100}, {"timestamp", 20000 + i}});
        QCOMPARE(model.rowCount(), 300);
        QJsonArray previous;
        for (int i = 0; i < 60; ++i)
            previous.append(
                QJsonObject{{"id", "page-" + QString::number(i)}, {"seq", 1000 + i}, {"timestamp", i}});
        model.prepend(previous);
        QCOMPARE(model.rowCount(), 300);
        QCOMPARE(model.rows.first()["id"].toString(), "page-0");
    }
    void sharedAvatarUsesSmallJpegAndRejectsInvalidData()
    {
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "JPEG"));
        QJsonObject entity{{"id", "avatar-test"}, {"name", "A"}};
        const auto fallback = chatAvatar(entity, 36).toImage();
        entity["avatar"] = QString::fromLatin1(jpeg.toBase64());
        const auto avatar = chatAvatar(entity, 36).toImage();
        QCOMPARE(avatar.size(), QSize(36, 36));
        const auto pixel = avatar.pixelColor(18, 6);
        QVERIFY(pixel.blue() > 200 && pixel.red() < 40 && pixel.green() < 40);
        QCOMPARE(avatar.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(chatAvatar(entity, 36).toImage(), avatar);
        QCOMPARE(chatAvatar(entity, 24).size(), QSize(24, 24));
        entity["avatar"] = "invalid base64!";
        QCOMPARE(chatAvatar(entity, 36).toImage(), fallback);
        jpeg.clear();
        buffer.seek(0);
        image = image.scaled(128, 128);
        QVERIFY(image.save(&buffer, "JPEG"));
        entity["avatar"] = QString::fromLatin1(jpeg.toBase64());
        QCOMPARE(chatAvatar(entity, 36).toImage(), fallback);
        entity["avatar"] = QString(40 * 1024, 'x');
        QCOMPARE(chatAvatar(entity, 36).toImage(), fallback);
    }
};
QTEST_MAIN(ClientTests)
#include "tests.moc"
