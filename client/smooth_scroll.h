#pragma once
#include <QAbstractItemView>
#include <QApplication>
#include <QScrollBar>
#include <QStyleHints>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <functional>

// Animate discrete wheel ticks only. Trackpads retain the platform's pixel scrolling and inertia.
class SmoothScroll final : public QObject
{
  public:
    explicit SmoothScroll(QAbstractItemView *view) : QObject(view), view(view), animation(this)
    {
        animation.setObjectName("smoothScrollAnimation");
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
                    if (!writing)
                        stop();
                });
        connect(view->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this](int minimum, int maximum)
                {
                    // Appending a message must not cancel an upward wheel gesture in progress.
                    if (animation.state() == QAbstractAnimation::Running &&
                        (animation.endValue().toReal() < minimum || animation.endValue().toReal() > maximum))
                        stop();
                });
        connect(&animation, &QVariantAnimation::finished, this, [this] { boundary(); });
    }
    void stop() { animation.stop(); }
    bool isRunning() const { return animation.state() == QAbstractAnimation::Running; }
    std::function<void()> reachedTop, reachedBottom;

  protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Hide)
            stop();
        if (event->type() != QEvent::Wheel)
            return false;
        const auto wheel = static_cast<QWheelEvent *>(event);
        if (!wheel->pixelDelta().isNull() || wheel->angleDelta().y() == 0 ||
            wheel->modifiers().testFlag(Qt::ControlModifier) ||
            wheel->modifiers().testFlag(Qt::ShiftModifier))
        {
            stop();
            return false;
        }
        auto *bar = view->verticalScrollBar();
        const qreal delta = -wheel->angleDelta().y() / 120.0 *
                            qMax(1, QApplication::styleHints()->wheelScrollLines()) *
                            qMax(16, view->fontMetrics().height());
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
    bool writing = false, upward = false;
};
