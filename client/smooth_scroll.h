#pragma once
#include <QAbstractItemView>
#include <QApplication>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSettings>
#include <QStyleHints>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <functional>

// Damped wheel scrolling. Discrete notches and precise (driver-smoothed) wheels both go
// through the same animation; a real trackpad pan keeps the platform's own pixel scrolling
// and inertia, because its events carry a scroll phase and ours never do.
//
// The damping follows Godot 4.7's macOS model (see platform/macos/godot_content_view.mm):
// a notch moves page/8 * 0.3, and precise deltas are attenuated by precisely 0.03 of their
// raw pixels before the widget's own scale. Raw 1:1 pixel scrolling is far too fast for a
// wheel that reports precise deltas.
class SmoothScroll final : public QObject
{
  public:
    explicit SmoothScroll(QAbstractItemView *view) : QObject(view), view(view), animation(this)
    {
        animation.setObjectName("smoothScrollAnimation");
        setObjectName("smoothScroll");
        animation.setDuration(160);
        animation.setEasingCurve(QEasingCurve::OutCubic);
        view->viewport()->installEventFilter(this);
        view->verticalScrollBar()->installEventFilter(this);
        connect(&animation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value)
                {
                    writing = true;
                    this->view->verticalScrollBar()->setValue(qRound(value.toReal()));
                    writing = false;
                });
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int)
                {
                    if (!writing && !layoutChanging)
                        stop();
                });
        connect(view->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this](int minimum, int maximum)
                {
                    // Appending a message must not cancel an upward wheel gesture in progress.
                    if (!layoutChanging && animation.state() == QAbstractAnimation::Running &&
                        (animation.endValue().toReal() < minimum || animation.endValue().toReal() > maximum))
                        stop();
                });
        connect(&animation, &QVariantAnimation::finished, this, [this] { boundary(); });
    }
    void stop() { animation.stop(); }
    // 1.0 scrolls precise deltas 1:1 (the platform default); lower values damp them.
    void setDamping(qreal value) { preciseDamping = qBound(qreal(0.05), value, qreal(1.0)); }
    static qreal savedDamping()
    {
        // The environment variable is an escape hatch for judging the feel without a rebuild.
        const auto override = qEnvironmentVariable("WECHAT_LITE_WHEEL_DAMPING");
        const auto value = override.isEmpty() ? QSettings().value("wheelDamping", 0.35).toReal()
                                             : override.toDouble();
        return qBound(qreal(0.05), value, qreal(1.0));
    }
    // Applies a changed preference to every live view without restarting the app.
    static void refreshAll()
    {
        const auto damping = savedDamping();
        for (auto *widget : QApplication::topLevelWidgets())
            for (auto *child : widget->findChildren<QObject *>("smoothScroll"))
                static_cast<SmoothScroll *>(child)->setDamping(damping);
    }
    bool isRunning() const { return animation.state() == QAbstractAnimation::Running; }
    void setLayoutChanging(bool changing) { layoutChanging = changing; }
    void offsetBy(int pixels)
    {
        if (!pixels || !isRunning())
            return;
        // Re-layout moved the document, not the user's intended remaining wheel distance.
        const QSignalBlocker blocker(&animation);
        const auto elapsed = animation.currentTime();
        const auto start = animation.startValue().toReal() + pixels;
        const auto end = animation.endValue().toReal() + pixels;
        animation.setStartValue(start);
        animation.setEndValue(qBound(qreal(view->verticalScrollBar()->minimum()), end,
                                    qreal(view->verticalScrollBar()->maximum())));
        animation.setCurrentTime(elapsed);
    }
    std::function<void()> reachedTop, reachedBottom;

  protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Hide)
            stop();
        if (event->type() != QEvent::Wheel)
            return false;
        const auto wheel = static_cast<QWheelEvent *>(event);
        if (wheel->angleDelta().y() == 0 || wheel->modifiers().testFlag(Qt::ControlModifier) ||
            wheel->modifiers().testFlag(Qt::ShiftModifier) || isTrackpadPan(*wheel))
        {
            stop();
            return false;
        }
        auto *bar = view->verticalScrollBar();
        const auto delta = travel(*wheel);
        if (delta == 0)
        {
            stop();
            boundary();
            wheel->accept();
            return true;
        }
        upward = delta < 0;
        const auto current = bar->value();
        const auto previous = animation.endValue().toReal();
        const bool continuing =
            animation.state() == QAbstractAnimation::Running && (previous - current) * delta > 0;
        const auto target =
            qBound(qreal(bar->minimum()), (continuing ? previous : current) + delta, qreal(bar->maximum()));
        stop();
        if (qRound(target) == current)
            boundary();
        else
        {
            animation.setStartValue(qreal(current));
            animation.setEndValue(target);
            animation.start();
        }
        wheel->accept();
        return true;
    }

  private:
    // A real trackpad pan is phase-tagged; a wheel never is, not even a driver-smoothed
    // one that reports precise pixel deltas. Only the former keeps native inertia.
    static bool isTrackpadPan(const QWheelEvent &wheel)
    {
        return wheel.phase() != Qt::NoScrollPhase;
    }
    // Damping follows Godot 4.7 on macOS: a discrete notch travels page/8 * 0.3, which keeps
    // the speed tied to the viewport instead of leaping a fixed number of rows. Precise
    // (driver-smoothed) deltas are then attenuated with the same 0.3 factor rather than
    // being scrolled 1:1, which is what made the wheel uncontrollable.
    qreal travel(const QWheelEvent &wheel) const
    {
        const auto notch = qMax(1, view->verticalScrollBar()->pageStep()) / 8.0 * 0.3;
        if (!wheel.pixelDelta().isNull())
            return -wheel.pixelDelta().y() * preciseDamping;
        return -qRound(wheel.angleDelta().y() / 120.0 * notch);
    }
    void boundary()
    {
        auto *bar = view->verticalScrollBar();
        if (upward && bar->value() <= 40 && reachedTop)
            reachedTop();
        else if (!upward && bar->value() >= bar->maximum() && reachedBottom)
            reachedBottom();
    }
    QAbstractItemView *view;
    QVariantAnimation animation;
    // One wheel notch arrives as ~119 px of precise deltas on driver-smoothed wheels, so 0.35
    // puts a notch near 42 px (about two lines). Overridable from Preferences; the environment
    // variable stays as an escape hatch for testing without a settings round-trip.
    qreal preciseDamping = savedDamping();
    bool writing = false, upward = false, layoutChanging = false;
};
