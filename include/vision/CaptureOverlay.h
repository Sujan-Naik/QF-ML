#ifndef CAPTUREOVERLAY_H
#define CAPTUREOVERLAY_H

#include <QWidget>
#include <QTimer>
#include <QRect>

class CaptureOverlay : public QWidget {
    Q_OBJECT

public:
    explicit CaptureOverlay(QWidget *parent = nullptr);

    ~CaptureOverlay();

    void startScan(const QRect &globalTargetRect);

    void stopScan();

signals:
    void textExtracted(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QTimer *m_animTimer = nullptr;
    QRect m_targetRect;
    int m_scanLineY = 0;
    bool m_isScanning = false;
};

#endif // CAPTUREOVERLAY_H
