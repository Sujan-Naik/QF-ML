#include "ui/ModelDialog.h"

#include "../../include/inference/InferenceService.h"
#include "app/QfPaths.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
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
    QString formatSize(qint64 bytes) {
        if (bytes <= 0) {
            return QStringLiteral("Size pending");
        }

        const double gb = static_cast<double>(bytes) / 1000000000.0;
        if (gb >= 1.0) {
            return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
        }

        const double mb = static_cast<double>(bytes) / 1000000.0;
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 0);
    }

    QString formatDownloads(qint64 downloads) {
        if (downloads >= 1000000) {
            return QStringLiteral("%1M").arg(
                static_cast<double>(downloads) / 1000000.0,
                0,
                'f',
                1
            );
        }

        if (downloads >= 1000) {
            return QStringLiteral("%1K").arg(
                static_cast<double>(downloads) / 1000.0,
                0,
                'f',
                1
            );
        }

        return QString::number(downloads);
    }
}

ModelDialog::ModelDialog(
    InferenceService *inferenceService,
    QWidget *parent
)
    : QDialog(parent)
    , m_inference(inferenceService) {
    setWindowTitle(QStringLiteral("QF-ML — Local Models"));
    resize(1100, 800);

    auto *root = new QVBoxLayout(this);

    auto *directoryLayout = new QHBoxLayout();
    directoryLayout->addWidget(
        new QLabel(QStringLiteral("Model directory:"), this)
    );

    m_directoryLabel = new QLabel(this);
    m_directoryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_directoryButton = new QPushButton(QStringLiteral("Choose..."), this);
    m_directoryButton->setAutoDefault(false);
    m_directoryButton->setDefault(false);

    directoryLayout->addWidget(m_directoryLabel, 1);
    directoryLayout->addWidget(m_directoryButton);
    root->addLayout(directoryLayout);

    auto *localHeaderLayout = new QHBoxLayout();
    localHeaderLayout->addWidget(
        new QLabel(QStringLiteral("Local installed models"), this)
    );
    localHeaderLayout->addStretch();

    m_refreshLocalButton = new QPushButton(QStringLiteral("Refresh"), this);
    localHeaderLayout->addWidget(m_refreshLocalButton);
    root->addLayout(localHeaderLayout);

    m_localModelList = new QListWidget(this);
    m_localModelList->setMinimumHeight(150);
    root->addWidget(m_localModelList);

    m_localModelInfoLabel = new QLabel(this);
    m_localModelInfoLabel->setWordWrap(true);
    m_localModelInfoLabel->setMinimumHeight(70);
    root->addWidget(m_localModelInfoLabel);

    root->addWidget(
        new QLabel(
            QStringLiteral("Download models from Hugging Face"),
            this
        )
    );

    auto *searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(
        QStringLiteral("Search Hugging Face...")
    );

    m_searchButton = new QPushButton(QStringLiteral("Search"), this);
    m_recommendedButton = new QPushButton(QStringLiteral("Recommended"), this);
    m_quantizationGuideButton = new QPushButton(
        QStringLiteral("Quantization Guide"),
        this
    );

    m_vramFilter = new QComboBox(this);
    m_vramFilter->addItem(QStringLiteral("Any VRAM"), 0);
    m_vramFilter->addItem(QStringLiteral("≤ 6 GB"), 6);
    m_vramFilter->addItem(QStringLiteral("≤ 8 GB"), 8);
    m_vramFilter->addItem(QStringLiteral("≤ 12 GB"), 12);
    m_vramFilter->addItem(QStringLiteral("≤ 16 GB"), 16);
    m_vramFilter->addItem(QStringLiteral("≤ 24 GB"), 24);
    m_vramFilter->addItem(QStringLiteral("≤ 32 GB"), 32);
    m_vramFilter->addItem(QStringLiteral("≤ 48 GB"), 48);
    m_vramFilter->addItem(QStringLiteral("≤ 64 GB"), 64);
    m_vramFilter->addItem(QStringLiteral("≤ 96 GB"), 96);
    m_vramFilter->addItem(QStringLiteral("≤ 128 GB"), 128);

    searchLayout->addWidget(m_searchEdit, 1);
    searchLayout->addWidget(m_recommendedButton);
    searchLayout->addWidget(m_searchButton);
    searchLayout->addWidget(m_quantizationGuideButton);
    searchLayout->addWidget(m_vramFilter);
    root->addLayout(searchLayout);

    auto *listsLayout = new QHBoxLayout();

    auto *repositoryLayout = new QVBoxLayout();
    repositoryLayout->addWidget(
        new QLabel(QStringLiteral("Hugging Face repositories"), this)
    );
    m_repositoryList = new QListWidget(this);
    repositoryLayout->addWidget(m_repositoryList);

    auto *variantLayout = new QVBoxLayout();
    variantLayout->addWidget(
        new QLabel(QStringLiteral("GGUF variants"), this)
    );
    m_variantList = new QListWidget(this);
    variantLayout->addWidget(m_variantList);

    listsLayout->addLayout(repositoryLayout, 1);
    listsLayout->addLayout(variantLayout, 1);
    root->addLayout(listsLayout, 1);

    m_modelInfoLabel = new QLabel(this);
    m_modelInfoLabel->setWordWrap(true);
    m_modelInfoLabel->setMinimumHeight(120);
    root->addWidget(m_modelInfoLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    auto *actionsLayout = new QHBoxLayout();
    m_downloadButton = new QPushButton(QStringLiteral("Download"), this);
    m_selectButton = new QPushButton(QStringLiteral("Use Selected"), this);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), this);

    actionsLayout->addWidget(m_downloadButton);
    actionsLayout->addWidget(m_selectButton);
    actionsLayout->addStretch();
    actionsLayout->addWidget(closeButton);
    root->addLayout(actionsLayout);

    connect(
        m_directoryButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::chooseDirectory
    );
    connect(
        m_refreshLocalButton,
        &QPushButton::clicked,
        this,
        &ModelDialog::refreshLocalModels
    );
    connect(
        m_localModelList,
        &QListWidget::itemSelectionChanged,
        this,
        &ModelDialog::localModelSelectionChanged
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
        [this]() { populateVariants(); }
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
                m_directoryLabel->setText(QDir(directory).absolutePath());
                populateLocalModels();
            }
        );
        connect(
            m_inference,
            &InferenceService::selectedLlmModelChanged,
            this,
            [this]() {
                populateLocalModels();
                updateLocalModelInfo();
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
            QDir(m_inference->modelDirectory()).absolutePath()
        );
        populateLocalModels();
    }

    updateLocalModelInfo();
    updateButtons();
}

void ModelDialog::openQuantizationGuide() {
    QuantizationGuideDialog guide(this);
    guide.exec();
}

void ModelDialog::search() {
    if (!m_inference) {
        return;
    }

    m_repositoryList->clear();
    m_variantList->clear();
    m_modelInfoLabel->clear();

    m_statusLabel->setText(QStringLiteral("Searching Hugging Face..."));
    m_inference->searchLlmModels(m_searchEdit->text().trimmed());
}

void ModelDialog::recommended() {
    m_searchEdit->setText(QStringLiteral("Qwen Coder"));
    search();
}

void ModelDialog::chooseDirectory() {
    if (!m_inference) {
        return;
    }

    const QString directory = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Choose Model Directory"),
        m_inference->modelDirectory()
    );

    if (directory.isEmpty()) {
        return;
    }

    if (!m_inference->setModelDirectory(directory)) {
        m_statusLabel->setText(
            QStringLiteral("Could not use the selected directory.")
        );
        return;
    }

    populateLocalModels();
}

void ModelDialog::repositorySelectionChanged() {
    const QString repoId = selectedRepoId();

    m_variantList->clear();
    m_modelInfoLabel->clear();

    if (repoId.isEmpty()) {
        updateButtons();
        return;
    }

    m_statusLabel->setText(QStringLiteral("Inspecting repository..."));
    m_inference->inspectLlmModel(repoId);
}

void ModelDialog::variantSelectionChanged() {
    if (m_variantList->currentItem()) {
        m_localModelList->clearSelection();
    }
    updateVariantInfo();
    updateButtons();
}

void ModelDialog::localModelSelectionChanged() {
    if (m_localModelList->currentItem()) {
        m_repositoryList->clearSelection();
        m_variantList->clearSelection();
        m_modelInfoLabel->clear();
    }
    updateLocalModelInfo();
    updateButtons();
}

void ModelDialog::refreshLocalModels() {
    populateLocalModels();
    updateLocalModelInfo();

    if (m_inference) {
        m_statusLabel->setText(QStringLiteral("Local model list refreshed."));
    }
}

void ModelDialog::populateLocalModels() {
    if (!m_inference) {
        return;
    }

    const QString previousPath = selectedLocalModelPath();
    m_localModelList->clear();

    const QDir directory(QFPaths::llmModelsDir());
    if (!directory.exists()) {
        m_localModelInfoLabel->setText(
            QStringLiteral("LLM model directory does not exist.")
        );
        return;
    }

    const QStringList filters = {
        QStringLiteral("*.gguf"),
        QStringLiteral("*.GGUF"),
        QStringLiteral("*.bin"),
        QStringLiteral("*.BIN"),
        QStringLiteral("*.safetensors"),
        QStringLiteral("*.SAFETENSORS")
    };

    QDirIterator iterator(
        directory.absolutePath(),
        filters,
        QDir::Files | QDir::Readable,
        QDirIterator::Subdirectories
    );

    const QString activeModelId = m_inference->selectedLlmModelId();
    int modelCount = 0;

    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo fileInfo(path);

        const bool active =
            !activeModelId.isEmpty() &&
            (activeModelId == path || activeModelId == fileInfo.fileName());

        const QString status = active ? QStringLiteral("ACTIVE")
                                      : QStringLiteral("INSTALLED");

        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n   %2 • %3").arg(
                fileInfo.fileName(),
                formatSize(fileInfo.size()),
                status
            )
        );

        item->setData(Qt::UserRole, path);
        item->setSizeHint(QSize(0, 58));

        m_localModelList->addItem(item);

        if (path == previousPath || active) {
            m_localModelList->setCurrentItem(item);
        }

        ++modelCount;
    }

    if (modelCount == 0) {
        m_localModelInfoLabel->setText(
            QStringLiteral("No supported local LLM model files found.")
        );
        return;
    }

    m_localModelInfoLabel->setText(
        QStringLiteral("%1 local LLM model file%2 found.")
            .arg(modelCount)
            .arg(modelCount == 1 ? QString() : QStringLiteral("s"))
    );
}

void ModelDialog::updateLocalModelInfo() {
    if (!m_localModelList) {
        return;
    }

    const QString path = selectedLocalModelPath();
    if (path.isEmpty()) {
        if (m_localModelList->count() == 0) {
            m_localModelInfoLabel->setText(
                QStringLiteral("No local model selected.")
            );
        }
        return;
    }

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        m_localModelInfoLabel->setText(
            QStringLiteral("The selected local model no longer exists.")
        );
        return;
    }

    const QString activeModelId = m_inference
        ? m_inference->selectedLlmModelId()
        : QString();

    const bool active =
        activeModelId == path || activeModelId == fileInfo.fileName();

    const QString status = active ? QStringLiteral("ACTIVE")
                                  : QStringLiteral("INSTALLED");

    m_localModelInfoLabel->setText(
        QStringLiteral(
            "<b>%1</b><br>"
            "Path: %2<br>"
            "Size: %3<br>"
            "Status: <b>%4</b>"
        ).arg(
            fileInfo.fileName(),
            fileInfo.absoluteFilePath(),
            formatSize(fileInfo.size()),
            status
        )
    );
}

void ModelDialog::populateRepositories() {
    if (!m_inference) {
        return;
    }

    const QString previousRepo = selectedRepoId();
    m_repositoryList->clear();

    const auto models = m_inference->remoteLlmModels();
    for (const auto &model : models) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n   %2 downloads • %3").arg(
                model.id,
                formatDownloads(model.downloads),
                model.lastModified
            )
        );

        item->setData(Qt::UserRole, model.id);
        item->setSizeHint(QSize(0, 58));

        m_repositoryList->addItem(item);

        if (model.id == previousRepo) {
            m_repositoryList->setCurrentItem(item);
        }
    }

    m_statusLabel->setText(
        QStringLiteral("%1 Hugging Face GGUF repositories found.")
            .arg(models.size())
    );

    updateButtons();
}

void ModelDialog::populateVariants() {
    if (!m_inference) {
        return;
    }

    const QString repoId = selectedRepoId();
    if (repoId.isEmpty()) {
        return;
    }

    const auto variants = m_inference->remoteLlmVariants(repoId);
    const int vramLimit = m_vramFilter->currentData().toInt();
    const QString selectedModelId = m_inference->selectedLlmModelId();
    const QString previousVariantId = selectedVariantId();

    m_variantList->clear();

    for (const auto &variant : variants) {
        if (vramLimit > 0 && variant.estimatedVramGb > 0.0 &&
            variant.estimatedVramGb > static_cast<double>(vramLimit)) {
            continue;
        }

        const bool installed =
            m_inference->models()->isVariantInstalled(variant);
        const bool active = variant.id == selectedModelId;

        QString status;
        if (active) {
            status = QStringLiteral("ACTIVE");
        } else if (installed) {
            status = QStringLiteral("INSTALLED");
        } else {
            status = QStringLiteral("DOWNLOAD");
        }

        const QString size = variant.sizeBytes > 0
            ? formatSize(variant.sizeBytes)
            : QStringLiteral("size pending");

        const QString vram = variant.estimatedVramGb > 0.0
            ? QStringLiteral("~%1 GB VRAM").arg(variant.estimatedVramGb, 0, 'f', 1)
            : QStringLiteral("VRAM pending");

        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n   %2 • %3 • %4").arg(
                variant.quantization,
                size,
                vram,
                status
            )
        );

        item->setData(Qt::UserRole, variant.id);
        item->setSizeHint(QSize(0, 62));

        m_variantList->addItem(item);

        if (variant.id == previousVariantId || active) {
            m_variantList->setCurrentItem(item);
        }
    }

    updateVariantInfo();
    updateButtons();
}

void ModelDialog::onRemoteModelsChanged() {
    populateRepositories();
}

void ModelDialog::onRemoteVariantsChanged(const QString &repoId) {
    if (repoId != selectedRepoId()) {
        return;
    }

    populateVariants();
}

QString ModelDialog::selectedRepoId() const {
    const QListWidgetItem *item = m_repositoryList->currentItem();
    if (!item) {
        return QString();
    }

    return item->data(Qt::UserRole).toString();
}

QString ModelDialog::selectedVariantId() const {
    const QListWidgetItem *item = m_variantList->currentItem();
    if (!item) {
        return QString();
    }

    return item->data(Qt::UserRole).toString();
}

QString ModelDialog::selectedLocalModelPath() const {
    const QListWidgetItem *item = m_localModelList->currentItem();
    if (!item) {
        return QString();
    }

    return item->data(Qt::UserRole).toString();
}

void ModelDialog::updateVariantInfo() {
    if (!m_inference) {
        m_modelInfoLabel->clear();
        return;
    }

    const QString repoId = selectedRepoId();
    const QString variantId = selectedVariantId();

    if (repoId.isEmpty() || variantId.isEmpty()) {
        m_modelInfoLabel->clear();
        return;
    }

    const auto variants = m_inference->remoteLlmVariants(repoId);
    for (const auto &variant : variants) {
        if (variant.id != variantId) {
            continue;
        }

        const bool installed =
            m_inference->models()->isVariantInstalled(variant);
        const bool active =
            variant.id == m_inference->selectedLlmModelId();

        QString status;
        if (active) {
            status = QStringLiteral("ACTIVE");
        } else if (installed) {
            status = QStringLiteral("INSTALLED");
        } else {
            status = QStringLiteral("DOWNLOAD FROM HUGGING FACE");
        }

        const QString size = variant.sizeBytes > 0
            ? formatSize(variant.sizeBytes)
            : QStringLiteral("Size pending");

        const QString vram = variant.estimatedVramGb > 0.0
            ? QStringLiteral("~%1 GB").arg(variant.estimatedVramGb, 0, 'f', 1)
            : QStringLiteral("Pending");

        const QString files = variant.fileNames.isEmpty()
            ? QStringLiteral("Unknown")
            : QString::number(variant.fileNames.size());

        m_modelInfoLabel->setText(
            QStringLiteral(
                "<b>%1</b><br>"
                "Repository: %2<br>"
                "Quantization: %3<br>"
                "Download: %4<br>"
                "Estimated full-GPU VRAM: %5<br>"
                "GGUF files: %6<br>"
                "Status: <b>%7</b><br><br>"
                "Use the Quantization Guide if the raw quantization name is unfamiliar."
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

void ModelDialog::updateButtons() {
    if (!m_inference) {
        m_downloadButton->setEnabled(false);
        m_selectButton->setEnabled(false);
        return;
    }

    const QString localPath = selectedLocalModelPath();
    const QString repoId = selectedRepoId();
    const QString variantId = selectedVariantId();

    bool canDownload = false;
    bool canSelect = false;

    if (!localPath.isEmpty()) {
        canSelect = QFileInfo::exists(localPath);
    } else if (!repoId.isEmpty() && !variantId.isEmpty()) {
        const auto variants = m_inference->remoteLlmVariants(repoId);
        for (const auto &variant : variants) {
            if (variant.id == variantId) {
                canDownload = true;
                canSelect = m_inference->models()->isVariantInstalled(variant);
                break;
            }
        }
    }

    m_downloadButton->setEnabled(canDownload);
    m_selectButton->setEnabled(canSelect);
}

bool ModelDialog::selectLocalModelPath(const QString &path) {
    if (!m_inference || path.isEmpty()) {
        return false;
    }

    if (!m_inference->selectLlmModel(path)) {
        m_statusLabel->setText(
            QStringLiteral("Could not activate selected local model file.")
        );
        return false;
    }

    m_statusLabel->setText(
        QStringLiteral("Local model selected. Starting inference...")
    );

    populateLocalModels();
    updateLocalModelInfo();
    return true;
}

void ModelDialog::downloadSelected() {
    if (!m_inference) {
        return;
    }

    const QString repoId = selectedRepoId();
    const QString variantId = selectedVariantId();

    if (repoId.isEmpty() || variantId.isEmpty()) {
        return;
    }

    const auto variants = m_inference->remoteLlmVariants(repoId);
    for (const auto &variant : variants) {
        if (variant.id == variantId) {
            m_statusLabel->setText(
                QStringLiteral("Downloading %1 from Hugging Face...")
                    .arg(variant.displayName)
            );

            m_inference->downloadLlmModel(variant);
            return;
        }
    }
}

void ModelDialog::selectSelected() {
    if (!m_inference) {
        return;
    }

    const QString localPath = selectedLocalModelPath();
    if (!localPath.isEmpty()) {
        selectLocalModelPath(localPath);
        return;
    }

    const QString repoId = selectedRepoId();
    const QString variantId = selectedVariantId();

    if (repoId.isEmpty() || variantId.isEmpty()) {
        return;
    }

    const auto variants = m_inference->remoteLlmVariants(repoId);
    for (const auto &variant : variants) {
        if (variant.id == variantId) {
            if (!m_inference->selectLlmModel(variant)) {
                m_statusLabel->setText(
                    QStringLiteral("This model is not installed yet.")
                );
                return;
            }

            m_statusLabel->setText(
                QStringLiteral("Model selected. Starting local inference...")
            );

            populateLocalModels();
            updateLocalModelInfo();
            return;
        }
    }
}

void ModelDialog::onDownloadStarted(const QString &modelId) {
    Q_UNUSED(modelId);

    m_progress->setVisible(true);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    updateButtons();
}

void ModelDialog::onDownloadProgress(
    const QString &modelId,
    qint64 received,
    qint64 total
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(true);

    if (total <= 0) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 100);
        const int percent = static_cast<int>(
            (static_cast<double>(received) / static_cast<double>(total)) * 100.0
        );
        m_progress->setValue(percent);
    }
}

void ModelDialog::onDownloadFinished(const QString &modelId) {
    Q_UNUSED(modelId);

    m_progress->setVisible(false);
    m_statusLabel->setText(QStringLiteral("Download complete."));

    populateLocalModels();
    updateLocalModelInfo();
    populateVariants();
    updateVariantInfo();
    updateButtons();
}

void ModelDialog::onDownloadError(
    const QString &modelId,
    const QString &error
) {
    Q_UNUSED(modelId);

    m_progress->setVisible(false);
    m_statusLabel->setText(
        QStringLiteral("Download failed: %1").arg(error)
    );

    updateButtons();
}