#include "../../include/voice/VisemeMap.h"

#include <QHash>
#include <QRegularExpression>

namespace {

// Weight of each viseme class when distributing a word's duration
// across its phonemes. Vowels hold, fricatives hold less, stops are
// brief, silence is minimal. These are the numbers to tune first if
// the mouth feels too fast or too slow.
double weightForViseme(const QString &code) {
  if (code == QStringLiteral("aa") || code == QStringLiteral("E") ||
      code == QStringLiteral("I") || code == QStringLiteral("O") ||
      code == QStringLiteral("U")) {
    return 1.0;
  }

  if (code == QStringLiteral("SS") || code == QStringLiteral("TH") ||
      code == QStringLiteral("FF") || code == QStringLiteral("CH")) {
    return 0.75;
  }

  if (code == QStringLiteral("nn") || code == QStringLiteral("RR")) {
    return 0.6;
  }

  if (code == QStringLiteral("PP") || code == QStringLiteral("kk") ||
      code == QStringLiteral("DD")) {
    return 0.4;
  }

  if (code == QStringLiteral("sil")) {
    return 0.2;
  }

  return 0.6;
}

// Kokoro emits IPA with stress marks and length marks. Strip them
// before lookup so the table does not need a variant per stressed
// syllable.
QString stripDiacritics(const QString &phoneme) {
  QString out;
  out.reserve(phoneme.size());

  for (QChar c : phoneme) {
    const ushort u = c.unicode();

    // U+02C8 primary stress, U+02CC secondary stress, U+02D0 length,
    // U+02D1 half length.
    if (u == 0x02C8 || u == 0x02CC || u == 0x02D0 || u == 0x02D1) {
      continue;
    }

    out.append(c);
  }

  return out;
}

// The IPA characters Kokoro can emit. Used to split a word's phoneme
// run into individual phonemes. Multi-character entries are not
// needed: Kokoro emits one IPA code point per phoneme, plus the
// diacritics stripped above.
bool isPhonemeChar(QChar c) {
  const ushort u = c.unicode();

  // Basic Latin letters used in IPA (a-z, plus ɡ U+0261, ɹ U+0279,
  // ʃ U+0283, ʒ U+0292, θ U+03B8, ð U+00F0, ŋ U+014B, ʔ U+0294).
  if (u >= 'a' && u <= 'z') return true;

  switch (u) {
  case 0x0261: // ɡ
  case 0x0279: // ɹ
  case 0x0283: // ʃ
  case 0x0292: // ʒ
  case 0x03B8: // θ
  case 0x00F0: // ð
  case 0x014B: // ŋ
  case 0x0294: // ʔ
  case 0x0259: // ə
  case 0x025A: // ɚ
  case 0x025B: // ɛ
  case 0x0254: // ɔ
  case 0x028A: // ʊ
  case 0x028C: // ʌ
  case 0x00E6: // æ
  case 0x0251: // ɑ
  case 0x0252: // ɒ
  case 0x026A: // ɪ
  case 0x0288: // ʈ
  case 0x0256: // ɖ
  case 0x026B: // ɫ
  case 0x027E: // ɾ
    return true;

  default:
    return false;
  }
}

QHash<QString, QString> buildTable() {
  QHash<QString, QString> table;

  // Vowels.
  table.insert(QStringLiteral("a"),  QStringLiteral("aa"));
  table.insert(QStringLiteral("ɑ"), QStringLiteral("aa"));
  table.insert(QStringLiteral("æ"), QStringLiteral("aa"));
  table.insert(QStringLiteral("ʌ"), QStringLiteral("aa"));
  table.insert(QStringLiteral("ə"), QStringLiteral("aa"));
  table.insert(QStringLiteral("ɐ"), QStringLiteral("aa"));

  table.insert(QStringLiteral("ɛ"), QStringLiteral("E"));
  table.insert(QStringLiteral("e"), QStringLiteral("E"));

  table.insert(QStringLiteral("i"), QStringLiteral("I"));
  table.insert(QStringLiteral("ɪ"), QStringLiteral("I"));
  table.insert(QStringLiteral("j"), QStringLiteral("I"));

  table.insert(QStringLiteral("o"), QStringLiteral("O"));
  table.insert(QStringLiteral("ɔ"), QStringLiteral("O"));
  table.insert(QStringLiteral("ɒ"), QStringLiteral("O"));

  table.insert(QStringLiteral("u"), QStringLiteral("U"));
  table.insert(QStringLiteral("ʊ"), QStringLiteral("U"));
  table.insert(QStringLiteral("w"), QStringLiteral("U"));
  table.insert(QStringLiteral("ɚ"), QStringLiteral("U"));

  // Bilabial and labiodental stops and fricatives — lips closed.
  table.insert(QStringLiteral("p"), QStringLiteral("PP"));
  table.insert(QStringLiteral("b"), QStringLiteral("PP"));
  table.insert(QStringLiteral("m"), QStringLiteral("PP"));
  table.insert(QStringLiteral("f"), QStringLiteral("FF"));
  table.insert(QStringLiteral("v"), QStringLiteral("FF"));

  // Dental fricatives.
  table.insert(QStringLiteral("θ"), QStringLiteral("TH"));
  table.insert(QStringLiteral("ð"), QStringLiteral("TH"));

  // Sibilants and affricates.
  table.insert(QStringLiteral("s"), QStringLiteral("SS"));
  table.insert(QStringLiteral("z"), QStringLiteral("SS"));
  table.insert(QStringLiteral("ʃ"), QStringLiteral("CH"));
  table.insert(QStringLiteral("ʒ"), QStringLiteral("CH"));
  table.insert(QStringLiteral("tʃ"), QStringLiteral("CH"));
  table.insert(QStringLiteral("dʒ"), QStringLiteral("CH"));

  // Velar and alveolar stops — small opening.
  table.insert(QStringLiteral("k"), QStringLiteral("kk"));
  table.insert(QStringLiteral("ɡ"), QStringLiteral("kk"));
  table.insert(QStringLiteral("g"), QStringLiteral("kk"));
  table.insert(QStringLiteral("t"), QStringLiteral("DD"));
  table.insert(QStringLiteral("d"), QStringLiteral("DD"));
  table.insert(QStringLiteral("ʔ"), QStringLiteral("DD"));

  // Nasals and liquids — small opening.
  table.insert(QStringLiteral("n"), QStringLiteral("nn"));
  table.insert(QStringLiteral("ŋ"), QStringLiteral("nn"));
  table.insert(QStringLiteral("ɹ"), QStringLiteral("RR"));
  table.insert(QStringLiteral("r"), QStringLiteral("RR"));
  table.insert(QStringLiteral("l"), QStringLiteral("nn"));
  table.insert(QStringLiteral("ɫ"), QStringLiteral("nn"));

  // Everything else collapses to a neutral small opening.
  table.insert(QStringLiteral("h"), QStringLiteral("sil"));
  table.insert(QStringLiteral("ɾ"), QStringLiteral("DD"));
  table.insert(QStringLiteral("ʈ"), QStringLiteral("DD"));
  table.insert(QStringLiteral("ɖ"), QStringLiteral("DD"));

  return table;
}

const QHash<QString, QString> &table() {
  static const QHash<QString, QString> t = buildTable();
  return t;
}

} // namespace

QString VisemeMap::phonemeToViseme(const QString &phoneme) {
  const QString key = stripDiacritics(phoneme).trimmed().toLower();

  if (key.isEmpty()) {
    return QStringLiteral("sil");
  }

  const auto &t = table();
  const auto it = t.constFind(key);

  if (it != t.constEnd()) {
    return it.value();
  }

  // Multi-code-point phonemes Kokoro sometimes emits as one unit.
  if (key.startsWith(QStringLiteral("t")) &&
      key.contains(QChar(0x0283))) {
    return QStringLiteral("CH");
  }
  if (key.startsWith(QStringLiteral("d")) &&
      key.contains(QChar(0x0292))) {
    return QStringLiteral("CH");
  }

  return QStringLiteral("sil");
}

QStringList VisemeMap::splitWords(const QString &phonemeString) {
  static const QRegularExpression whitespace(QStringLiteral("\\s+"));

  QStringList words = phonemeString.trimmed().split(
      whitespace, Qt::SkipEmptyParts);

  return words;
}

QStringList VisemeMap::splitPhonemes(const QString &word) {
  QStringList out;

  const QString stripped = stripDiacritics(word);

  for (QChar c : stripped) {
    if (isPhonemeChar(c)) {
      out.append(QString(c));
    }
  }

  return out;
}

QVector<Viseme> VisemeMap::buildWordTimeline(const QString &wordPhonemes,
                                             int wordStartMs,
                                             int wordEndMs) {
  QVector<Viseme> out;

  if (wordEndMs <= wordStartMs) {
    return out;
  }

  const QStringList phonemes = splitPhonemes(wordPhonemes);

  if (phonemes.isEmpty()) {
    Viseme v;
    v.startMs = wordStartMs;
    v.endMs = wordEndMs;
    v.shape = QStringLiteral("sil");
    out.append(v);
    return out;
  }

  // Compute per-phoneme weight, then map each phoneme's slice.
  double totalWeight = 0.0;
  QVector<QString> codes;
  QVector<double> weights;

  codes.reserve(phonemes.size());
  weights.reserve(phonemes.size());

  for (const QString &p : phonemes) {
    const QString code = phonemeToViseme(p);
    const double w = weightForViseme(code);

    codes.append(code);
    weights.append(w);

    totalWeight += w;
  }

  if (totalWeight <= 0.0) {
    totalWeight = 1.0;
  }

  const int span = wordEndMs - wordStartMs;
  int cursor = wordStartMs;

  for (int i = 0; i < codes.size(); ++i) {
    const double fraction = weights.at(i) / totalWeight;
    const int duration = qMax(1, static_cast<int>(fraction * span));

    Viseme v;
    v.startMs = cursor;
    v.endMs = (i == codes.size() - 1) ? wordEndMs : cursor + duration;
    v.shape = codes.at(i);

    out.append(v);

    cursor = v.endMs;
  }

  return out;
}

QVector<Viseme> VisemeMap::buildTimeline(const QStringList &wordPhonemes,
                                         const QVector<int> &startMs,
                                         const QVector<int> &endMs) {
  QVector<Viseme> out;

  const int count = qMin(wordPhonemes.size(),
                         qMin(startMs.size(), endMs.size()));

  out.reserve(count * 4);

  for (int i = 0; i < count; ++i) {
    const QVector<Viseme> wordVisemes =
        buildWordTimeline(wordPhonemes.at(i), startMs.at(i), endMs.at(i));

    out.append(wordVisemes);
  }

  return out;
}