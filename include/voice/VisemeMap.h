#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// A single mouth shape with a time range, relative to the start of the
// audio in the enclosing AudioChunk. Times are milliseconds.
//
// shape is one of the Oculus 15 viseme codes Kokoro's phonemes map to:
//
//   aa  E   I   O   U   PP  SS  TH  CH  FF  kk  nn  RR  DD  sil
//
// The five vowel codes drive the visible mouth shapes on the avatar.
// The consonant codes drive a closed or near-closed mouth. sil is
// neutral.
struct Viseme {
  int startMs = 0;
  int endMs = 0;
  QString shape;
};

struct AudioChunk {
  QByteArray data;
  int sampleRate = 24000;
  int speakerId = 0;
  qint64 timestamp = 0;

  // Populated by the captioned speech path. Empty when the chunk came
  // from the plain synthesis path.
  QVector<Viseme> visemes;
};

// Maps Kokoro's IPA phonemes to Oculus 15 viseme codes, and distributes
// a word's duration across its phonemes to produce a viseme timeline.
class VisemeMap {
public:
  // Map a single IPA phoneme (without stress marks) to its Oculus
  // code. Returns "sil" for anything unrecognised.
  static QString phonemeToViseme(const QString &phoneme);

  // Split a phoneme string produced by Kokoro's /dev/phonemize into
  // words. Kokoro returns space-separated words.
  static QStringList splitWords(const QString &phonemeString);

  // Split a single word's phoneme run into individual IPA phonemes.
  static QStringList splitPhonemes(const QString &word);

  // Build a viseme timeline for one word. The word's phonemes are
  // distributed across [wordStartMs, wordEndMs] by weight: vowels hold
  // longer, stops are brief.
  static QVector<Viseme> buildWordTimeline(const QString &wordPhonemes,
                                           int wordStartMs, int wordEndMs);

  // Build a full timeline from parallel arrays of phoneme words and
  // timings. All three must be the same length; the shorter is used.
  static QVector<Viseme> buildTimeline(const QStringList &wordPhonemes,
                                       const QVector<int> &startMs,
                                       const QVector<int> &endMs);
};