#ifndef OCRSERVICE_H
#define OCRSERVICE_H

#include <QImage>
#include <QString>

class OcrService {
public:
    static QString extractText(const QImage &srcImage);
};

#endif
