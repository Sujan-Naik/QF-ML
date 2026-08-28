#include "../../include/vision/OcrService.h"
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

QString OcrService::extractText(const QImage &srcImage) {
    if (srcImage.isNull()) return QString();

    QImage processedImage = srcImage.scaled(srcImage.width() * 2, srcImage.height() * 2,
                                            Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);

    tesseract::TessBaseAPI tess;
    if (tess.Init(nullptr, "eng", tesseract::OEM_LSTM_ONLY)) {
        return QString();
    }

    tess.SetPageSegMode(tesseract::PSM_AUTO);
    tess.SetImage(processedImage.bits(), processedImage.width(), processedImage.height(), 3,
                  processedImage.bytesPerLine());

    char *outText = tess.GetUTF8Text();
    QString result = QString::fromUtf8(outText).trimmed();

    delete[] outText;
    tess.End();

    return result;
}
