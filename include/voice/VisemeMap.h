#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// A single mouth shape with a time range, relative to the start of the
// audio in the enclosing AudioChunk. Times are milliseconds.
//
// shape is one of the Oculus 15 viseme codes HeadTTS emits:
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

  QVector<Viseme> visemes;
};

// HeadTTS returns viseme timing as three parallel arrays: viseme codes,
// start times in ms, and durations in ms. This zips them into the
// AudioChunk's Viseme vector. All three arrays must be the same length;
// the shorter is used.
//
// The audio samples themselves arrive base64-encoded in the same JSON
// response and are decoded by the caller, not here.
class VisemeMap {
public:
  static QVector<Viseme> buildTimeline(const QStringList &visemes,
                                       const QVector<int> &vtimes,
                                       const QVector<int> &vdurations);
};