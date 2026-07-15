#include "line_icons.hpp"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QGuiApplication>
#include <QScreen>

namespace quizpane::ui {

QIcon makeLineIcon(LineIcon type) {
    const qreal ratio = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(190, 197, 205, 220), 1.5,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    switch (type) {
    case LineIcon::Previous:
        path.moveTo(15, 5); path.lineTo(8, 12); path.lineTo(15, 19); path.closeSubpath();
        painter.drawPath(path);
        break;
    case LineIcon::Next:
        path.moveTo(9, 5); path.lineTo(16, 12); path.lineTo(9, 19); path.closeSubpath();
        painter.drawPath(path);
        break;
    case LineIcon::Submit:
        painter.drawRoundedRect(QRectF(3.5, 3.5, 10, 16), 1.5, 1.5);
        painter.drawLine(QPointF(6, 8), QPointF(11, 8));
        painter.drawLine(QPointF(6, 11), QPointF(10, 11));
        path.moveTo(13, 18); path.lineTo(13, 12); path.cubicTo(13, 10, 15, 10, 15, 12);
        path.lineTo(15, 8); path.cubicTo(15, 6.5, 17, 6.5, 17, 8);
        path.lineTo(17, 13); path.lineTo(19, 11); path.cubicTo(20, 10, 21.5, 11.5, 20.5, 13);
        path.lineTo(18, 18); path.closeSubpath();
        painter.drawPath(path);
        break;
    case LineIcon::Pin:
        path.moveTo(8, 5); path.lineTo(16, 5); path.lineTo(14, 10);
        path.lineTo(17, 13); path.lineTo(7, 13); path.lineTo(10, 10);
        path.closeSubpath(); painter.drawPath(path);
        painter.drawLine(QPointF(12, 13), QPointF(12, 20));
        break;
    case LineIcon::Resize:
        painter.drawLine(QPointF(5, 10), QPointF(5, 5));
        painter.drawLine(QPointF(5, 5), QPointF(10, 5));
        painter.drawLine(QPointF(19, 14), QPointF(19, 19));
        painter.drawLine(QPointF(19, 19), QPointF(14, 19));
        painter.drawLine(QPointF(8, 8), QPointF(16, 16));
        break;
    case LineIcon::Close:
        painter.drawLine(QPointF(7, 7), QPointF(17, 17));
        painter.drawLine(QPointF(17, 7), QPointF(7, 17));
        break;
    case LineIcon::Catalog:
        for (int y : {6, 12, 18}) {
            painter.drawEllipse(QPointF(6, y), 1, 1);
            painter.drawLine(QPointF(10, y), QPointF(18, y));
        }
        break;
    case LineIcon::Menu:
        painter.drawLine(QPointF(5, 7), QPointF(19, 7));
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
        painter.drawLine(QPointF(5, 17), QPointF(19, 17));
        break;
    case LineIcon::ChevronUp:
        path.moveTo(6, 14); path.lineTo(12, 8); path.lineTo(18, 14);
        painter.drawPath(path);
        break;
    case LineIcon::ChevronDown:
        path.moveTo(6, 10); path.lineTo(12, 16); path.lineTo(18, 10);
        painter.drawPath(path);
        break;
    }
    return QIcon(pixmap);
}

}  // namespace quizpane::ui
