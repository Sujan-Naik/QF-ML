#pragma once

#include <QDialog>

class QuantizationGuideDialog : public QDialog {
    Q_OBJECT

public:
    explicit QuantizationGuideDialog(
        QWidget *parent = nullptr
    );
};
