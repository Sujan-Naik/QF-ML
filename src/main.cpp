#include <QApplication>
#include <QDebug>

#include "inference/InferenceService.h"
#include "inference/LlamaManager.h"
#include "ui/ModelDialog.h"


static LlamaManager::Backend configuredLlamaBackend() {
    const QString backend =
            qEnvironmentVariable(
                "TALOS_LLM_BACKEND"
            ).trimmed().toLower();

    if (backend == QStringLiteral("rocm"))
        return LlamaManager::Backend::Rocm;

    if (backend == QStringLiteral("cuda"))
        return LlamaManager::Backend::Cuda;

    if (backend == QStringLiteral("vulkan"))
        return LlamaManager::Backend::Vulkan;

    if (backend == QStringLiteral("intel"))
        return LlamaManager::Backend::Intel;

    /*
     * AMD/Linux default.
     */
    return LlamaManager::Backend::Vulkan;
}

int main(int argc, char *argv[]) {

    QCoreApplication::setOrganizationName("QuestFarer");
    QCoreApplication::setApplicationName("qf-ml");
    QCoreApplication::setApplicationVersion("0.0");


    qputenv(
        "QTWEBENGINE_CHROMIUM_FLAGS",
        "--no-sandbox "
        "--disable-gpu-sandbox "
        "--disable-gpu-compositing "
        "--disable-seccomp-filter-sandbox"
    );

    qputenv(
        "QTWEBENGINE_REMOTE_DEBUGGING",
        "9223"
    );

    QCoreApplication::setAttribute(
        Qt::AA_ShareOpenGLContexts
    );

    int fakeArgc =
            argc + 1;

    char **fakeArgv =
            new char *[fakeArgc];

    for (int i = 0; i < argc; ++i)
        fakeArgv[i] = argv[i];

    char noSandboxFlag[] =
            "--no-sandbox";

    fakeArgv[argc] =
            noSandboxFlag;

    QApplication app(
        fakeArgc,
        fakeArgv
    );

    InferenceService inferenceService;

    const QString sttModelPath = qEnvironmentVariable("TALOS_STT_MODEL"
    ).trimmed();

    if (
        !inferenceService.initialize(
            configuredLlamaBackend(),
            sttModelPath
        )
    ) {
        qWarning()
                << "[Talos] Inference service initialization failed.";
    }

    QObject::connect(
        &inferenceService,
        &InferenceService::serviceError,
        [](const QString &error) {
            qWarning()
                    << "[InferenceService]"
                    << error;
        }
    );

    ModelDialog window(
        &inferenceService
    );

    window.show();

    const int result =
            app.exec();

    delete[] fakeArgv;

    return result;
}
