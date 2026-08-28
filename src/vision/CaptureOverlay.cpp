#include "vision/CaptureOverlay.h"

#include <QPainter>
#include <QLinearGradient>
#include <QScreen>
#include <QGuiApplication>


CaptureOverlay::CaptureOverlay(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

    m_animTimer = new QTimer(this);
    connect(m_animTimer, &QTimer::timeout, this, [this]() {
        if (!m_isScanning || m_targetRect.isEmpty()) return;

        m_scanLineY += 4; // Animation speed
        if (m_scanLineY > m_targetRect.bottom()) {
            m_scanLineY = m_targetRect.top();
        }
        update(); // Request repaint for scanline animation
    });
}

CaptureOverlay::~CaptureOverlay() {
    stopScan();
}

void CaptureOverlay::startScan(const QRect &globalTargetRect) {
    if (globalTargetRect.isEmpty()) return;

    m_targetRect = globalTargetRect;
    m_scanLineY = m_targetRect.top();
    m_isScanning = true;

    // Cover the target screen
    QScreen *screen = QGuiApplication::screenAt(m_targetRect.center());
    if (!screen) screen = QGuiApplication::primaryScreen();

    setGeometry(screen->geometry());
    show();
    raise();

    m_animTimer->start(16); // ~60 FPS
}

void CaptureOverlay::stopScan() {
    // 1. Stop timer loop
    if (m_animTimer && m_animTimer->isActive()) {
        m_animTimer->stop();
    }

    // 2. Clear state flags
    m_isScanning = false;
    m_scanLineY = 0;
    m_targetRect = QRect();

    // 3. Hide overlay and force immediate repaint clear
    hide();
    update();
}

void CaptureOverlay::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    if (!m_isScanning || m_targetRect.isEmpty()) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Map global target rectangle into local overlay coordinates
    QRect localRect = QRect(mapFromGlobal(m_targetRect.topLeft()), m_targetRect.size());

    // Draw scanning bounding box
    QPen pen(QColor(0, 122, 255, 200), 2, Qt::DashLine);
    painter.setPen(pen);
    painter.drawRect(localRect);

    // Draw active animated laser line
    int localScanY = mapFromGlobal(QPoint(0, m_scanLineY)).y();
    if (localScanY >= localRect.top() && localScanY <= localRect.bottom()) {
        QLinearGradient grad(localRect.left(), localScanY, localRect.right(), localScanY);
        grad.setColorAt(0.0, QColor(0, 122, 255, 0));
        grad.setColorAt(0.5, QColor(0, 212, 255, 255));
        grad.setColorAt(1.0, QColor(0, 122, 255, 0));

        QPen linePen(QBrush(grad), 3);
        painter.setPen(linePen);
        painter.drawLine(localRect.left(), localScanY, localRect.right(), localScanY);
    }
}
