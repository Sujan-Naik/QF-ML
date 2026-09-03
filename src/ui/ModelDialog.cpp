#include "ui/ModelDialog.h"

#include "../../include/inference/InferenceService.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/QuantizationGuideDialog.h"


namespace {
    QString formatSize(
        qint64 bytes
    ) {
        if (bytes <= 0)
            return QStringLiteral(
                "Size pending"
            );

        const double gb =
                static_cast<double>(
                    bytes
                ) /
                1000000000.0;

        if (gb >= 1.0) {
            return QStringLiteral(
                "%1 GB"
            ).arg(
                gb,
                0,
                'f',
                2
            );
        }

        const double mb =
                static_cast<double>(
                    bytes
                ) /
                1000000.0;

        return QStringLiteral(
            "%1 MB"
        ).arg(
            mb,
            0,
            'f',
            0
        );
    }


    QString formatDownloads(
        qint64 downloads
    ) {
        if (downloads >= 1000000) {
            return QStringLiteral(
                "%1M"
            ).arg(
                static_cast<double>(
                    downloads
                ) /
                1000000.0,
                0,
                'f',
                1
            );
        }

        if (downloads >= 1000) {
            return QStringLiteral(
                "%1K"
            ).arg(
                static_cast<double>(
                    downloads
                ) /
                1000.0,
                0,
                'f',
                1
            );
        }

        return QString::number(
            downloads
        );
    }
}


ModelDialog::ModelDialog(
    InferenceService *inferenceService,
    QWidget *parent
)
    : QDialog(parent)
      , m_inference(inferenceService) {
    setWindowTitle(
        QStringLiteral(
            "QF-ML — Local Models"
        )
    );

    resize(
        1000,
        700
    );

    auto *root =
            new QVBoxLayout(
                this
            );

    // -------------------------------------------------------------------------
    // Model directory
    // -------------------------------------------------------------------------

    auto *directoryLayout =
            new QHBoxLayout();

    directoryLayout->addWidget(
        new QLabel(
            QStringLiteral(
                "Model directory:"
            ),
            this
        )
    );

    m_directoryLabel =
            new QLabel(
                this
            );

    m_directoryLabel
            ->setTextInteractionFlags(
                Qt::TextSelectableByMouse
            );

    m_directoryButton =
            new QPushButton(
                QStringLiteral(
                    "Choose..."
                ),
                this
            );

    directoryLayout->addWidget(
        m_directoryLabel,
        1
    );

    directoryLayout->addWidget(
        m_directoryButton
    );

    root->addLayout(
        directoryLayout
    );

    // -------------------------------------------------------------------------
    // Search
    // -------------------------------------------------------------------------

    auto *searchLayout =
            new QHBoxLayout();

    m_searchEdit =
            new QLineEdit(
                this
            );

    m_searchEdit->setPlaceholderText(
        QStringLiteral(
            "Search Hugging Face..."
        )
    );

    m_searchButton =
            new QPushButton(
                QStringLiteral(
                    "Search"
                ),
                this
            );

    m_recommendedButton =
            new QPushButton(
                QStringLiteral(
                    "Recommended"
                ),
                this
            );

    m_quantizationGuideButton =
            new QPushButton(
                QStringLiteral(
                    "Quantization Guide"
                ),
                this
            );

    m_vramFilter =
            new QComboBox(
                this
            );

    m_vramFilter->addItem(
        QStringLiteral(
            "Any VRAM"
        ),
        0
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 6 GB"
        ),
        6
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 8 GB"
        ),
        8
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 12 GB"
        ),
        12
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 16 GB"
        ),
        16
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 24 GB"
        ),
        24
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 32 GB"
        ),
        32
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 48 GB"
        ),
        48
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 64 GB"
        ),
        64
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 96 GB"
        ),
        96
    );

    m_vramFilter->addItem(
        QStringLiteral(
            "≤ 128 GB"
        ),
        128
    );

    searchLayout->addWidget(
        m_searchEdit,
        1
    );

    searchLayout->addWidget(
        m_recommendedButton
    );

    searchLayout->addWidget(
        m_searchButton
    );

    searchLayout->addWidget(
        m_quantizationGuideButton
    );

    searchLayout->addWidget(
        m_vramFilter
    );

    root->addLayout(
        searchLayout
    );

    // -------------------------------------------------------------------------
    // Repository / variant lists
    // -------------------------------------------------------------------------

    auto *listsLayout =
            new QHBoxLayout();

    auto *repositoryLayout =
            new QVBoxLayout();

    repositoryLayout->addWidget(
        new QLabel(
            QStringLiteral(
                "Hugging Face repositories"
            ),
            this
        )
    );

    m_repositoryList =
            new QListWidget(
                this
            );

    repositoryLayout->addWidget(
        m_repositoryList
    );

    auto *variantLayout =
            new QVBoxLayout();

    variantLayout->addWidget(
        new QLabel(
            QStringLiteral(
                "GGUF variants"
            ),
            this
        )
    );

    m_variantList =
            new QListWidget(
                this
            );

    variantLayout->addWidget(
        m_variantList
    );

    listsLayout->addLayout(
        repositoryLayout,
        1
    );

    listsLayout->addLayout(
        variantLayout,
        1
    );

    root->addLayout(
        listsLayout,
        1
    );

    // -------------------------------------------------------------------------
    // Variant information
    // -------------------------------------------------------------------------

    m_modelInfoLabel =
            new QLabel(
                this
            );

    m_modelInfoLabel->setWordWrap(
        true
    );

    m_modelInfoLabel->setMinimumHeight(
        120
    );

    root->addWidget(
        m_modelInfoLabel
    );

    m_statusLabel =
            new QLabel(
                this
            );

    m_statusLabel->setWordWrap(
        true
    );

    root->addWidget(
        m_statusLabel
    );

    // -------------------------------------------------------------------------
    // Download progress
    // -------------------------------------------------------------------------

    m_progress =
            new QProgressBar(
                this
            );

    m_progress->setVisible(
        false
    );

    root->addWidget(
        m_progress
    );

    // -------------------------------------------------------------------------
    // Actions
    // -------------------------------------------------------------------------

    auto *actionsLayout =
            new QHBoxLayout();

    m_downloadButton =
            new QPushButton(
                QStringLiteral(
                    "Download"
                ),
                this
            );

    m_selectButton =
            new QPushButton(
                QStringLiteral(
                    "Use Selected"
                ),
                this
            );

    auto *closeButton =
            new QPushButton(
                QStringLiteral(
                    "Close"
                ),
                this
            );

    actionsLayout->addWidget(
        m_downloadButton
    );

    actionsLayout->addWidget(
        m_selectButton
    );

    actionsLayout->addStretch();

    actionsLayout->addWidget(
        closeButton
    );

    root->addLayout(
        actionsLayout
    );

    // -------------------------------------------------------------------------
    // Connections
    // -------------------------------------------------------------------------

    connect(
        m_directoryButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::chooseDirectory
    );

    connect(
        m_searchEdit,
        &QLineEdit::returnPressed,
        this,
        &ModelDialog::search
    );

    connect(
        m_searchButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::search
    );

    connect(
        m_recommendedButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::recommended
    );

    connect(
        m_quantizationGuideButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::openQuantizationGuide
    );

    connect(
        m_vramFilter,
        &QComboBox::currentIndexChanged,
        this,
        [this]() {
            populateVariants();
        }
    );

    connect(
        m_repositoryList,
        &QListWidget::itemSelectionChanged,
        this,
        &ModelDialog::repositorySelectionChanged
    );

    connect(
        m_variantList,
        &QListWidget::itemSelectionChanged,
        this,
        &ModelDialog::variantSelectionChanged
    );

    connect(
        m_downloadButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::downloadSelected
    );

    connect(
        m_selectButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::selectSelected
    );

    connect(
        closeButton,
        &QPushButton::clicked,
        this,
        &QDialog::accept
    );

    if (m_inference) {
        connect(
            m_inference,
            &InferenceService::remoteLlmModelsChanged,
            this,
            &ModelDialog::onRemoteModelsChanged
        );

        connect(
            m_inference,
            &InferenceService::remoteLlmVariantsChanged,
            this,
            &ModelDialog::onRemoteVariantsChanged
        );

        connect(
            m_inference,
            &InferenceService::modelDirectoryChanged,
            this,
            [this](const QString &directory) {
                m_directoryLabel->setText(
                    QDir(
                        directory
                    ).absolutePath()
                );
            }
        );

        connect(
            m_inference,
            &InferenceService::selectedLlmModelChanged,
            this,
            [this]() {
                populateVariants();
                updateVariantInfo();
                updateButtons();
            }
        );

        connect(
            m_inference,
            &InferenceService::modelDownloadStarted,
            this,
            &ModelDialog::onDownloadStarted
        );

        connect(
            m_inference,
            &InferenceService::modelDownloadProgress,
            this,
            &ModelDialog::onDownloadProgress
        );

        connect(
            m_inference,
            &InferenceService::modelDownloadFinished,
            this,
            &ModelDialog::onDownloadFinished
        );

        connect(
            m_inference,
            &InferenceService::modelDownloadError,
            this,
            &ModelDialog::onDownloadError
        );

        m_directoryLabel->setText(
            QDir(
                m_inference
                ->modelDirectory()
            ).absolutePath()
        );
    }

    updateButtons();
}


void ModelDialog::openQuantizationGuide() {
    QuantizationGuideDialog guide(
        this
    );

    guide.exec();
}


void ModelDialog::search() {
    if (!m_inference)
        return;

    m_repositoryList->clear();
    m_variantList->clear();
    m_modelInfoLabel->clear();

    m_statusLabel->setText(
        QStringLiteral(
            "Searching Hugging Face..."
        )
    );

    m_inference->searchLlmModels(
        m_searchEdit
        ->text()
        .trimmed()
    );
}


void ModelDialog::recommended() {
    /*
     * This is a useful starting search, not a hardcoded
     * model recommendation list.
     */
    m_searchEdit->setText(
        QStringLiteral(
            "Qwen Coder"
        )
    );

    search();
}


void ModelDialog::chooseDirectory() {
    if (!m_inference)
        return;

    const QString directory =
            QFileDialog::getExistingDirectory(
                this,
                QStringLiteral(
                    "Choose Model Directory"
                ),
                m_inference
                ->modelDirectory()
            );

    if (directory.isEmpty())
        return;

    if (
        !m_inference
        ->setModelDirectory(
            directory
        )
    ) {
        m_statusLabel->setText(
            QStringLiteral(
                "Could not use the selected directory."
            )
        );
    }
}


void ModelDialog::repositorySelectionChanged() {
    const QString repoId =
            selectedRepoId();

    m_variantList->clear();
    m_modelInfoLabel->clear();

    if (repoId.isEmpty()) {
        updateButtons();
        return;
    }

    m_statusLabel->setText(
        QStringLiteral(
            "Inspecting repository..."
        )
    );

    m_inference->inspectLlmModel(
        repoId
    );
}


void ModelDialog::variantSelectionChanged() {
    updateVariantInfo();
    updateButtons();
}


void ModelDialog::populateRepositories() {
    if (!m_inference)
        return;

    const QString previousRepo =
            selectedRepoId();

    m_repositoryList->clear();

    const auto models =
            m_inference
            ->remoteLlmModels();

    for (
        const auto &model:
        models
    ) {
        auto *item =
                new QListWidgetItem(
                    QStringLiteral(
                        "%1\n"
                        "   %2 downloads • %3"
                    ).arg(
                        model.id,
                        formatDownloads(
                            model.downloads
                        ),
                        model.lastModified
                    )
                );

        item->setData(
            Qt::UserRole,
            model.id
        );

        item->setSizeHint(
            QSize(
                0,
                58
            )
        );

        m_repositoryList->addItem(
            item
        );

        if (
            model.id ==
            previousRepo
        ) {
            m_repositoryList
                    ->setCurrentItem(
                        item
                    );
        }
    }

    m_statusLabel->setText(
        QStringLiteral(
            "%1 Hugging Face GGUF repositories found."
        ).arg(
            models.size()
        )
    );

    updateButtons();
}


void ModelDialog::populateVariants() {
    if (!m_inference)
        return;

    const QString repoId =
            selectedRepoId();

    if (repoId.isEmpty())
        return;

    const auto variants =
            m_inference
            ->remoteLlmVariants(
                repoId
            );

    const int vramLimit =
            m_vramFilter
            ->currentData()
            .toInt();

    const QString selectedModelId =
            m_inference
            ->selectedLlmModelId();

    const QString previousVariantId =
            selectedVariantId();

    m_variantList->clear();

    for (
        const auto &variant:
        variants
    ) {
        if (
            vramLimit > 0 &&
            variant.estimatedVramGb > 0.0 &&
            variant.estimatedVramGb >
            static_cast<double>(
                vramLimit
            )
        ) {
            continue;
        }

        const bool installed =
                m_inference
                ->models()
                ->isVariantInstalled(
                    variant
                );

        const bool active =
                variant.id ==
                selectedModelId;

        QString status;

        if (active) {
            status =
                    QStringLiteral(
                        "ACTIVE"
                    );
        } else if (installed) {
            status =
                    QStringLiteral(
                        "INSTALLED"
                    );
        } else {
            status =
                    QStringLiteral(
                        "DOWNLOAD"
                    );
        }

        const QString size =
                variant.sizeBytes > 0
                    ? formatSize(
                        variant.sizeBytes
                    )
                    : QStringLiteral(
                        "size pending"
                    );

        const QString vram =
                variant.estimatedVramGb > 0.0
                    ? QStringLiteral(
                        "~%1 GB VRAM"
                    ).arg(
                        variant.estimatedVramGb,
                        0,
                        'f',
                        1
                    )
                    : QStringLiteral(
                        "VRAM pending"
                    );

        auto *item =
                new QListWidgetItem(
                    QStringLiteral(
                        "%1\n"
                        "   %2 • %3 • %4"
                    ).arg(
                        variant.quantization,
                        size,
                        vram,
                        status
                    )
                );

        item->setData(
            Qt::UserRole,
            variant.id
        );

        item->setSizeHint(
            QSize(
                0,
                62
            )
        );

        m_variantList->addItem(
            item
        );

        if (
            variant.id ==
            previousVariantId ||
            active
        ) {
            m_variantList
                    ->setCurrentItem(
                        item
                    );
        }
    }

    updateVariantInfo();
    updateButtons();
}


void ModelDialog::onRemoteModelsChanged() {
    populateRepositories();
}


void ModelDialog::onRemoteVariantsChanged(
    const QString &repoId
) {
    if (
        repoId !=
        selectedRepoId()
    ) {
        return;
    }

    populateVariants();
}


QString ModelDialog::selectedRepoId() const {
    const QListWidgetItem *item =
            m_repositoryList
            ->currentItem();

    if (!item)
        return QString();

    return item
            ->data(
                Qt::UserRole
            )
            .toString();
}


QString ModelDialog::selectedVariantId() const {
    const QListWidgetItem *item =
            m_variantList
            ->currentItem();

    if (!item)
        return QString();

    return item
            ->data(
                Qt::UserRole
            )
            .toString();
}


void ModelDialog::updateVariantInfo() {
    if (!m_inference) {
        m_modelInfoLabel->clear();
        return;
    }

    const QString repoId =
            selectedRepoId();

    const QString variantId =
            selectedVariantId();

    if (
        repoId.isEmpty() ||
        variantId.isEmpty()
    ) {
        m_modelInfoLabel->clear();
        return;
    }

    const auto variants =
            m_inference
            ->remoteLlmVariants(
                repoId
            );

    for (
        const auto &variant:
        variants
    ) {
        if (
            variant.id !=
            variantId
        ) {
            continue;
        }

        const bool installed =
                m_inference
                ->models()
                ->isVariantInstalled(
                    variant
                );

        const bool active =
                variant.id ==
                m_inference
                ->selectedLlmModelId();

        QString status;

        if (active) {
            status =
                    QStringLiteral(
                        "ACTIVE"
                    );
        } else if (installed) {
            status =
                    QStringLiteral(
                        "INSTALLED"
                    );
        } else {
            status =
                    QStringLiteral(
                        "DOWNLOAD FROM HUGGING FACE"
                    );
        }

        const QString size =
                variant.sizeBytes > 0
                    ? formatSize(
                        variant.sizeBytes
                    )
                    : QStringLiteral(
                        "Size pending"
                    );

        const QString vram =
                variant.estimatedVramGb > 0.0
                    ? QStringLiteral(
                        "~%1 GB"
                    ).arg(
                        variant.estimatedVramGb,
                        0,
                        'f',
                        1
                    )
                    : QStringLiteral(
                        "Pending"
                    );

        const QString files =
                variant.fileNames.isEmpty()
                    ? QStringLiteral(
                        "Unknown"
                    )
                    : QString::number(
                        variant.fileNames.size()
                    );

        m_modelInfoLabel->setText(
            QStringLiteral(
                "<b>%1</b><br>"
                "Repository: %2<br>"
                "Quantization: %3<br>"
                "Download: %4<br>"
                "Estimated full-GPU VRAM: %5<br>"
                "GGUF files: %6<br>"
                "Status: <b>%7</b><br><br>"
                "Use the Quantization Guide if the raw "
                "quantization name is unfamiliar."
            ).arg(
                variant.displayName,
                variant.repoId,
                variant.quantization,
                size,
                vram,
                files,
                status
            )
        );

        return;
    }

    m_modelInfoLabel->clear();
}


void ModelDialog::downloadSelected() {
    if (!m_inference)
        return;

    const QString repoId =
            selectedRepoId();

    const QString variantId =
            selectedVariantId();

    if (
        repoId.isEmpty() ||
        variantId.isEmpty()
    ) {
        return;
    }

    const auto variants =
            m_inference
            ->remoteLlmVariants(
                repoId
            );

    for (
        const auto &variant:
        variants
    ) {
        if (
            variant.id ==
            variantId
        ) {
            m_statusLabel->setText(
                QStringLiteral(
                    "Downloading %1 from Hugging Face..."
                ).arg(
                    variant.displayName
                )
            );

            m_inference
                    ->downloadLlmModel(
                        variant
                    );

            return;
        }
    }
}


void ModelDialog::selectSelected() {
    if (!m_inference)
        return;

    const QString repoId =
            selectedRepoId();

    const QString variantId =
            selectedVariantId();

    if (
        repoId.isEmpty() ||
        variantId.isEmpty()
    ) {
        return;
    }

    const auto variants =
            m_inference
            ->remoteLlmVariants(
                repoId
            );

    for (
        const auto &variant:
        variants
    ) {
        if (
            variant.id ==
            variantId
        ) {
            if (
                !m_inference
                ->selectLlmModel(
                    variant
                )
            ) {
                m_statusLabel->setText(
                    QStringLiteral(
                        "This model is not installed yet."
                    )
                );

                return;
            }

            m_statusLabel->setText(
                QStringLiteral(
                    "Model selected. Starting local inference..."
                )
            );

            return;
        }
    }
}


void ModelDialog::onDownloadStarted(
    const QString &modelId
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(
        true
    );

    m_progress->setRange(
        0,
        100
    );

    m_progress->setValue(
        0
    );

    updateButtons();
}


void ModelDialog::onDownloadProgress(
    const QString &modelId,
    qint64 received,
    qint64 total
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(
        true
    );

    if (
        total <= 0
    ) {
        m_progress->setRange(
            0,
            0
        );
    } else {
        m_progress->setRange(
            0,
            100
        );

        const int percent =
                static_cast<int>(
                    (
                        static_cast<double>(
                            received
                        ) /
                        static_cast<double>(
                            total
                        )
                    ) *
                    100.0
                );

        m_progress->setValue(
            qBound(
                0,
                percent,
                100
            )
        );

        m_statusLabel->setText(
            QStringLiteral(
                "Downloading — %1%"
            ).arg(
                percent
            )
        );
    }

    updateButtons();
}


void ModelDialog::onDownloadFinished(
    const QString &modelId
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(
        false
    );

    m_progress->setRange(
        0,
        100
    );

    m_statusLabel->setText(
        QStringLiteral(
            "Download complete. Local model selected."
        )
    );

    populateVariants();
    updateVariantInfo();
    updateButtons();
}


void ModelDialog::onDownloadError(
    const QString &modelId,
    const QString &error
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(
        false
    );

    m_progress->setRange(
        0,
        100
    );

    m_statusLabel->setText(
        QStringLiteral(
            "Download failed: %1"
        ).arg(
            error
        )
    );

    updateButtons();
}


void ModelDialog::updateButtons() {
    if (!m_inference) {
        m_downloadButton->setEnabled(
            false
        );

        m_selectButton->setEnabled(
            false
        );

        return;
    }

    const QString repoId =
            selectedRepoId();

    const QString variantId =
            selectedVariantId();

    bool installed =
            false;

    bool active =
            false;

    if (
        !repoId.isEmpty() &&
        !variantId.isEmpty()
    ) {
        const auto variants =
                m_inference
                ->remoteLlmVariants(
                    repoId
                );

        for (
            const auto &variant:
            variants
        ) {
            if (
                variant.id ==
                variantId
            ) {
                installed =
                        m_inference
                        ->models()
                        ->isVariantInstalled(
                            variant
                        );

                active =
                        variant.id ==
                        m_inference
                        ->selectedLlmModelId();

                break;
            }
        }
    }

    const bool downloading =
            m_inference
            ->models()
            ->isDownloading();

    m_downloadButton->setEnabled(
        !variantId.isEmpty() &&
        !installed &&
        !downloading
    );

    m_selectButton->setEnabled(
        !variantId.isEmpty() &&
        installed &&
        !active &&
        !downloading
    );
}
