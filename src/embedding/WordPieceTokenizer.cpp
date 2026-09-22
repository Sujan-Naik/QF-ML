#include "../../include/embedding/WordPieceTokenizer.h"

#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <cctype>

namespace {

constexpr int kMaxCharsPerWord = 200;

bool isPunctuation(QChar c) {
  // BERT's basic tokenizer treats a fixed set of ASCII punctuation as
  // split points. We approximate with the Unicode category check plus
  // the ASCII set that BERT actually splits on.
  static const QString kAsciiPunct =
      QStringLiteral("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
  if (kAsciiPunct.contains(c)) {
    return true;
  }
  return c.category() == QChar::Punctuation_Other ||
         c.category() == QChar::Punctuation_Dash ||
         c.category() == QChar::Punctuation_Open ||
         c.category() == QChar::Punctuation_Close ||
         c.category() == QChar::Punctuation_InitialQuote ||
         c.category() == QChar::Punctuation_FinalQuote ||
         c.category() == QChar::Punctuation_Connector ||
         c.category() == QChar::Symbol_Math;
}

QString stripAccents(const QString &text) {
  // Normalize to NFD so accented characters are decomposed into a base
  // character followed by combining marks, then strip the combining
  // marks. This matches BERT's accent stripping.
  const QString decomposed = text.normalized(QString::NormalizationForm_KD);

  QString out;
  out.reserve(decomposed.size());

  for (QChar c : decomposed) {
    if (c.category() == QChar::Mark_NonSpacing ||
        c.category() == QChar::Mark_SpacingCombining ||
        c.category() == QChar::Mark_Enclosing) {
      continue;
        }
    out.append(c);
  }

  return out;
}
} // namespace

bool WordPieceTokenizer::load(const QString &vocabPath) {
  m_vocab.clear();

  QFile file(vocabPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[WordPiece] Cannot open vocab:" << vocabPath;
    return false;
  }

  QTextStream stream(&file);
  stream.setEncoding(QStringConverter::Utf8);

  int id = 0;
  while (!stream.atEnd()) {
    const QString line = stream.readLine();
    // Vocab lines are tokens. Strip only the trailing newline, not
    // whitespace, because some tokens are whitespace-significant.
    m_vocab.emplace(line.toStdString(), id);
    ++id;
  }

  file.close();

  if (m_vocab.empty()) {
    qWarning() << "[WordPiece] Empty vocab:" << vocabPath;
    return false;
  }

  // Look up special token ids. Fall back to BERT defaults if absent.
  auto lookup = [this](const QString &token, int fallback) {
    auto it = m_vocab.find(token.toStdString());
    return it != m_vocab.end() ? it->second : fallback;
  };

  m_clsId = lookup(QStringLiteral("[CLS]"), 101);
  m_sepId = lookup(QStringLiteral("[SEP]"), 102);
  m_padId = lookup(QStringLiteral("[PAD]"), 0);
  m_unkId = lookup(QStringLiteral("[UNK]"), 100);

  qDebug() << "[WordPiece] Loaded vocab, size =" << m_vocab.size()
           << "CLS =" << m_clsId << "SEP =" << m_sepId
           << "UNK =" << m_unkId;

  return true;
}

QStringList WordPieceTokenizer::basicTokenize(const QString &text) const {
  // Lowercase first. MiniLM is uncased.
  QString working = text.toLower();

  // Strip accents. MiniLM's tokenizer does this.
  working = stripAccents(working);

  // Insert spaces around punctuation so the whitespace split below
  // separates it cleanly.
  QString spaced;
  spaced.reserve(working.size() * 2);
  for (QChar c : working) {
    if (isPunctuation(c)) {
      spaced.append(QLatin1Char(' '));
      spaced.append(c);
      spaced.append(QLatin1Char(' '));
    } else if (c.isSpace()) {
      spaced.append(QLatin1Char(' '));
    } else {
      spaced.append(c);
    }
  }

  QStringList tokens;
  static const QRegularExpression whitespace(QStringLiteral("\\s+"));
  tokens = spaced.split(whitespace, Qt::SkipEmptyParts);

  return tokens;
}

QVector<int64_t> WordPieceTokenizer::wordPiece(const QString &word) const {
  QVector<int64_t> ids;

  if (word.isEmpty()) {
    return ids;
  }

  const QString prepared = word.length() > kMaxCharsPerWord
                               ? word.left(kMaxCharsPerWord)
                               : word;

  int start = 0;
  const int length = prepared.length();

  while (start < length) {
    int end = length;
    QString current;

    while (start < end) {
      QString fragment = prepared.mid(start, end - start);
      if (start > 0) {
        fragment.prepend(QStringLiteral("##"));
      }

      auto it = m_vocab.find(fragment.toStdString());
      if (it != m_vocab.end()) {
        current = fragment;
        ids.append(it->second);
        break;
      }

      --end;
    }

    if (current.isEmpty()) {
      // No subword match. The whole remaining fragment is unknown.
      ids.append(m_unkId);
      return ids;
    }

    start = end;
  }

  return ids;
}

QVector<int64_t> WordPieceTokenizer::encode(const QString &text,
                                            int maxLength) const {
  QVector<int64_t> ids;

  if (maxLength < 2) {
    // Need room for at least [CLS] and [SEP].
    maxLength = 2;
  }

  ids.append(m_clsId);

  const QStringList words = basicTokenize(text);
  const int contentBudget = maxLength - 2;

  for (const QString &word : words) {
    const QVector<int64_t> pieces = wordPiece(word);
    for (int64_t piece : pieces) {
      if (ids.size() >= 1 + contentBudget) {
        break;
      }
      ids.append(piece);
    }
    if (ids.size() >= 1 + contentBudget) {
      break;
    }
  }

  ids.append(m_sepId);

  return ids;
}

QVector<int64_t> WordPieceTokenizer::attentionMask(int length) const {
  QVector<int64_t> mask;
  mask.reserve(length);
  for (int i = 0; i < length; ++i) {
    mask.append(1);
  }
  return mask;
}