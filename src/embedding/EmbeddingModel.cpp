#include "../../include/embedding/EmbeddingModel.h"

#include <QDebug>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

Ort::SessionOptions makeSessionOptions() {
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);
  return options;
}

} // namespace

EmbeddingModel::~EmbeddingModel() = default;

bool EmbeddingModel::load(const QString &modelPath, const QString &vocabPath) {
  if (!QFile::exists(modelPath)) {
    qWarning() << "[EmbeddingModel] Model file not found:" << modelPath;
    return false;
  }

  if (!m_tokenizer.load(vocabPath)) {
    qWarning() << "[EmbeddingModel] Failed to load vocab:" << vocabPath;
    return false;
  }

  try {
    Ort::SessionOptions options = makeSessionOptions();

    // ONNX Runtime on Linux takes const char*; on Windows it wants
    // wchar_t*. The WakeWordDetector already uses this pattern.
#ifdef _WIN32
    m_session = std::make_unique<Ort::Session>(
        m_env, modelPath.toStdWString().c_str(), options);
#else
    m_session = std::make_unique<Ort::Session>(
        m_env, modelPath.toStdString().c_str(), options);
#endif
  } catch (const Ort::Exception &e) {
    qWarning() << "[EmbeddingModel] Failed to create session:"
               << e.what();
    m_session.reset();
    return false;
  }

  // Discover input and output names. MiniLM has two inputs:
  // input_ids and attention_mask, and one output: last_hidden_state.
  Ort::AllocatorWithDefaultOptions allocator;

  const size_t inputCount = m_session->GetInputCount();
  if (inputCount < 2) {
    qWarning() << "[EmbeddingModel] Expected 2 inputs, got" << inputCount;
    m_session.reset();
    return false;
  }

  for (size_t i = 0; i < inputCount; ++i) {
    const std::string name =
        m_session->GetInputNameAllocated(i, allocator).get();

    if (name.find("attention") != std::string::npos) {
      m_attentionMaskName = name;
    } else {
      m_inputIdsName = name;
    }
  }

  const size_t outputCount = m_session->GetOutputCount();
  if (outputCount < 1) {
    qWarning() << "[EmbeddingModel] Expected 1 output";
    m_session.reset();
    return false;
  }

  m_outputName =
      m_session->GetOutputNameAllocated(0, allocator).get();

  if (m_inputIdsName.empty() || m_attentionMaskName.empty() ||
      m_outputName.empty()) {
    qWarning() << "[EmbeddingModel] Could not resolve tensor names";
    m_session.reset();
    return false;
  }

  qDebug() << "[EmbeddingModel] Loaded" << modelPath
           << "inputs:" << QString::fromStdString(m_inputIdsName)
           << QString::fromStdString(m_attentionMaskName)
           << "output:" << QString::fromStdString(m_outputName);

  return true;
}

std::vector<float> EmbeddingModel::embed(const QString &text) {
  if (!m_session) {
    return {};
  }

  const QVector<int64_t> inputIds =
      m_tokenizer.encode(text, m_maxTokens);

  if (inputIds.isEmpty()) {
    return {};
  }

  const QVector<int64_t> attentionMask =
      m_tokenizer.attentionMask(inputIds.size());

  return runInference(inputIds, attentionMask);
}

std::vector<float> EmbeddingModel::runInference(
    const QVector<int64_t> &inputIds,
    const QVector<int64_t> &attentionMask) {
  if (inputIds.size() != attentionMask.size() ||
      inputIds.isEmpty()) {
    return {};
  }

  const int64_t sequenceLength = inputIds.size();

  // ONNX Runtime needs mutable data pointers. Copy into vectors we own.
  std::vector<int64_t> ids(inputIds.begin(), inputIds.end());
  std::vector<int64_t> mask(attentionMask.begin(), attentionMask.end());

  Ort::MemoryInfo memoryInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  const int64_t inputShape[] = {1, sequenceLength};

  Ort::Value idsTensor = Ort::Value::CreateTensor<int64_t>(
      memoryInfo, ids.data(), ids.size(), inputShape, 2);

  Ort::Value maskTensor = Ort::Value::CreateTensor<int64_t>(
      memoryInfo, mask.data(), mask.size(), inputShape, 2);

  const char *inputNames[] = {m_inputIdsName.c_str(),
                              m_attentionMaskName.c_str()};
  const char *outputNames[] = {m_outputName.c_str()};

  std::vector<Ort::Value> outputs;

  try {
    outputs = m_session->Run(
        Ort::RunOptions{nullptr}, inputNames,
        std::array<Ort::Value, 2>{std::move(idsTensor),
                                  std::move(maskTensor)}
            .data(),
        2, outputNames, 1);
  } catch (const Ort::Exception &e) {
    qWarning() << "[EmbeddingModel] Inference failed:" << e.what();
    return {};
  }

  if (outputs.empty() || !outputs[0].IsTensor()) {
    qWarning() << "[EmbeddingModel] No output tensor";
    return {};
  }

  // Output shape is [1, sequenceLength, 384].
  const auto shapeInfo =
      outputs[0].GetTensorTypeAndShapeInfo();
  const std::vector<int64_t> shape = shapeInfo.GetShape();

  if (shape.size() != 3 || shape[2] != m_dimensions) {
    qWarning() << "[EmbeddingModel] Unexpected output shape";
    return {};
  }

  const float *hidden =
      outputs[0].GetTensorData<float>();

  const int64_t tokens = shape[1];

  // Mean pooling over the attention mask. Every token is 1 in our
  // mask because we do not pad, so the divisor is just tokens.
  std::vector<float> pooled(m_dimensions, 0.0f);

  for (int64_t t = 0; t < tokens; ++t) {
    const float *row = hidden + t * m_dimensions;
    for (int d = 0; d < m_dimensions; ++d) {
      pooled[d] += row[d];
    }
  }

  const float inv = 1.0f / static_cast<float>(std::max<int64_t>(1, tokens));
  for (int d = 0; d < m_dimensions; ++d) {
    pooled[d] *= inv;
  }

  // L2 normalize so cosine similarity is a dot product.
  float norm = 0.0f;
  for (float v : pooled) {
    norm += v * v;
  }
  norm = std::sqrt(norm);

  if (norm > 1e-9f) {
    const float invNorm = 1.0f / norm;
    for (float &v : pooled) {
      v *= invNorm;
    }
  }

  return pooled;
}