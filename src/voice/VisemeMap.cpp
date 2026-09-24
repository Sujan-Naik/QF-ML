#include "../../include/voice/VisemeMap.h"

QVector<Viseme> VisemeMap::buildTimeline(const QStringList &visemes,
                                         const QVector<int> &vtimes,
                                         const QVector<int> &vdurations) {
  QVector<Viseme> out;

  const int count =
      qMin(visemes.size(), qMin(vtimes.size(), vdurations.size()));

  out.reserve(count);

  for (int i = 0; i < count; ++i) {
    Viseme v;
    v.startMs = vtimes.at(i);
    v.endMs = vtimes.at(i) + vdurations.at(i);
    v.shape = visemes.at(i);

    out.append(v);
  }

  return out;
}