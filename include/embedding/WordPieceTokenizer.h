#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <unordered_map>

// Hand-rolled WordPiece tokenizer for BERT-family models.
//
// Greedy longest-match with "##" continuation for subwords. Handles
// lowercasing, basic punctuation splitting, and the standard BERT
// special tokens: [CLS], [SEP], [PAD], [UNK], [MASK].
//
// The vocabulary is a plain text file, one token per line. The line
// index is the token id. This is the format shipped with MiniLM and
// every BERT derivative.
class WordPieceTokenizer {
public:
  WordPieceTokenizer() = default;

  // Load vocab.txt. Returns false if the file cannot be read.
  bool load(const QString &vocabPath);

  bool isLoaded() const { return !m_vocab.empty(); }

  // Tokenize into ids. Adds [CLS] at the front and [SEP] at the back.
  // Truncates to maxLength tokens including the two special tokens.
  QVector<int64_t> encode(const QString &text, int maxLength = 256) const;

  // Attention mask for encode(). All ones for tokens we actually
  // produced; no padding is added here because the model accepts
  // variable-length sequences.
  QVector<int64_t> attentionMask(int length) const;

  int vocabSize() const { return static_cast<int>(m_vocab.size()); }
  int clsId() const { return m_clsId; }
  int sepId() const { return m_sepId; }
  int padId() const { return m_padId; }
  int unkId() const { return m_unkId; }

private:
  // Split on whitespace and punctuation, then lowercase.
  QStringList basicTokenize(const QString &text) const;

  // WordPiece on a single whitespace token. Returns subword ids.
  QVector<int64_t> wordPiece(const QString &word) const;

  std::unordered_map<std::string, int> m_vocab;

  int m_clsId = 101;
  int m_sepId = 102;
  int m_padId = 0;
  int m_unkId = 100;
};