// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
namespace motion {
inline QString angleText(double degrees) {
    const auto turns = std::trunc(degrees / 360.0);
    const auto remainder = degrees - turns * 360;
    return QString::number(turns, 'f', 0) + "x" + (remainder >= 0 ? "+" : "") +
           QLocale().toString(remainder, 'f', 1) + "°";
}
inline std::optional<double> angleValue(QString text) {
    text.remove(QChar(0x00b0));
    text.replace(QChar(0x2212), '-');
    text.replace(QLocale().decimalPoint(), ".");
    text = text.trimmed();
    static const QRegularExpression pattern(
        R"(^([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*x\s*([+-](?:\d+(?:\.\d*)?|\.\d+))?$)");
    auto match = pattern.match(text);
    bool ok = false;
    double value = match.hasMatch() ? match.captured(1).toDouble() * 360 +
                                          (match.captured(2).isEmpty() ? 0 : match.captured(2).toDouble())
                                    : text.toDouble(&ok);
    if ((!match.hasMatch() && !ok) || !std::isfinite(value))
        return {};
    return value;
}

inline QString frameText(qint64 frame, qint64 rate) {
    return QString("%1:%2:%3:%4")
        .arg(frame / (rate * 3600))
        .arg(frame / rate / 60 % 60, 2, 10, QChar('0'))
        .arg(frame / rate % 60, 2, 10, QChar('0'))
        .arg(frame % rate, 2, 10, QChar('0'));
}
class FrameNumber final : public QDoubleSpinBox {
  public:
    explicit FrameNumber(QWidget *parent) : QDoubleSpinBox(parent) {
        setDecimals(0);
        setRange(0, 100000000);
        setKeyboardTracking(false);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
    }
    void setRate(double fps) {
        rate_ = std::max<qint64>(1, std::llround(fps));
        if (!hasFocus())
            lineEdit()->setText(textFromValue(value()));
    }

  protected:
    QString textFromValue(double value) const override { return frameText(std::llround(value), rate_); }
    double valueFromText(const QString &text) const override { return parse(text).value_or(value()); }
    QValidator::State validate(QString &text, int &) const override {
        auto value = parse(text);
        return value && *value >= minimum() && *value <= maximum() ? QValidator::Acceptable
                                                                   : QValidator::Intermediate;
    }

  private:
    qint64 rate_ = 30;
    std::optional<double> parse(const QString &text) const {
        const auto parts = text.trimmed().split(':');
        if (parts.size() != 1 && parts.size() != 4)
            return {};
        double frame = 0;
        for (int i = 0; i < parts.size(); ++i) {
            bool ok;
            auto n = parts[i].toLongLong(&ok);
            if (!ok || n < 0 || (parts.size() == 4 && i > 0 && n >= (i == 3 ? rate_ : 60)))
                return {};
            frame = frame * (i == 3 ? rate_ : 60) + n;
        }
        return frame;
    }
};

inline QString compactFixedNumber(QString text, const QString &separator) {
    if (text.contains(separator)) {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith(separator))
            text.chop(separator.size());
    }
    return text;
}
class ScrubNumber final : public QDoubleSpinBox {
  public:
    std::function<void()> begin, finish, cancel, focused, dismiss;
    bool angle = false;
    std::function<void(double)> scrub;
    explicit ScrubNumber(QWidget *parent) : QDoubleSpinBox(parent) {
        lineEdit()->installEventFilter(this);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setKeyboardTracking(false);
        setMinimumWidth(40);
        setFrame(false);
        setStyleSheet("QDoubleSpinBox,QDoubleSpinBox "
                      "QLineEdit{min-height:0;padding:0;border:0;background:transparent;}");
        setFixedHeight(17);
        setToolTip("Type a value or drag horizontally. Shift: faster; Alt: finer; Escape: cancel.");
    }

  protected:
    QString textFromValue(double value) const override {
        if (angle)
            return angleText(value);
        return compactFixedNumber(QDoubleSpinBox::textFromValue(value), locale().decimalPoint());
    }
    double valueFromText(const QString &text) const override {
        return angle ? angleValue(text).value_or(value()) : QDoubleSpinBox::valueFromText(text);
    }
    QValidator::State validate(QString &text, int &pos) const override {
        if (!angle)
            return QDoubleSpinBox::validate(text, pos);
        const auto parsed = angleValue(text);
        return parsed && *parsed >= minimum() && *parsed <= maximum() ? QValidator::Acceptable
                                                                      : QValidator::Intermediate;
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == lineEdit()) {
            if (event->type() == QEvent::FocusIn && focused)
                focused();
            if (event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && dismiss) {
                dismiss();
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                auto *e = static_cast<QMouseEvent *>(event);
                if (e->button() == Qt::LeftButton) {
                    pressed_ = true;
                    start_ = e->globalPosition();
                    initial_ = value();
                }
            } else if (event->type() == QEvent::MouseMove && pressed_) {
                auto *e = static_cast<QMouseEvent *>(event);
                const double dx = e->globalPosition().x() - start_.x();
                if (!dragging_ && std::abs(dx) >= QApplication::startDragDistance()) {
                    dragging_ = true;
                    begin();
                }
                if (dragging_) {
                    double speed = e->modifiers() & Qt::ShiftModifier ? 10
                                   : e->modifiers() & Qt::AltModifier ? .1
                                                                      : 1;
                    scrub(std::clamp(initial_ + dx * speed, minimum(), maximum()));
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease) {
                pressed_ = false;
                if (dragging_) {
                    dragging_ = false;
                    finish();
                    return true;
                }
            } else if (event->type() == QEvent::KeyPress &&
                       static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                pressed_ = false;
                if (dragging_) {
                    dragging_ = false;
                    cancel();
                    return true;
                }
            }
        }
        return QDoubleSpinBox::eventFilter(object, event);
    }

  private:
    bool pressed_ = false, dragging_ = false;
    QPointF start_;
    double initial_ = 0;
};
} // namespace motion
