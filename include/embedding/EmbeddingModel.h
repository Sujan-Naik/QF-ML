#pragma once

#include "WordPieceTokenizer.h"

#include <QString>

#include <memory>
#include <vector>

#include <onnxruntime_cxx_api.h>

// Local sentence embedding model. Loads a quantized ONNX file and the
// matching WordPiece vocabulary, and produces 384-dimensional L2-
// normalized vectors.
//
// The model is all-MiniLM-L6-v2, INT8 quantized. Input is tokenized
// with [CLS] at the front and [SEP] at the back, truncated at 256
// tokens. Output is mean-pooled over the attention mask and then
// L2-normalized so cosine similarity is a dot product.
//
// The ONNX export declares three inputs: input_ids, attention_mask,
// token_type_ids. Ort requires every declared input to be supplied,
// so a zero-filled token_type_ids tensor is always provided.
class EmbeddingModel {
public:
  EmbeddingModel() = default;
  ~EmbeddingModel();

  EmbeddingModel(const EmbeddingModel &) = delete;
  EmbeddingModel &operator=(const EmbeddingModel &) = delete;

  // Load the ONNX model and the vocab. Both paths must exist.
  bool load(const QString &modelPath, const QString &vocabPath);

  bool isLoaded() const { return m_session != nullptr; }

  // Produce an embedding. Returns an empty vector on failure.
  std::vector<float> embed(const QString &text);

  // Dimension of the vectors this model produces. 384 for MiniLM.
  int dimensions() const { return m_dimensions; }

  // Maximum number of tokens accepted, including the two special
  // tokens. MiniLM is 256.
  int maxTokens() const { return m_maxTokens; }

private:
  std::vector<float> runInference(const QVector<int64_t> &inputIds,
                                  const QVector<int64_t> &attentionMask);

  Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "LoreEmbedding"};
  std::unique_ptr<Ort::Session> m_session;

  WordPieceTokenizer m_tokenizer;

  std::string m_inputIdsName;
  std::string m_attentionMaskName;
  std::string m_tokenTypeIdsName;
  std::string m_outputName;

  int m_dimensions = 384;
  int m_maxTokens = 256;
};