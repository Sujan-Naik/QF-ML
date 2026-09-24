#pragma once

#include <QHash>
#include <QString>
#include <QVector>

// Maps an Oculus 15 viseme name onto one or more Reallusion shape key
// weights on the CC Base face mesh.
//
// HeadTTS returns Oculus 15: sil PP FF TH DD kk CH SS nn RR aa E I O U.
// The CC Base exposes Reallusion's own jaw and mouth shapes. Several
// Oculus visemes share a shape and are differentiated by weight.
//
// The table is built once at load time against the actual morph
// target names present on the face mesh, so a target that the export
// did not carry is simply skipped, not an error.
class VisemeMap {
public:
  struct Weight {
    int morphIndex = -1;
    float weight = 0.0f;
  };

  // Build the table. nameToIndex maps a morph target name to its
  // index in the face mesh primitive's morphTargetNames list. The
  // index space is per-primitive and the CC Base face mesh has six
  // primitives, all sharing the same 148 targets, so index 0 of any
  // primitive is the same logical target.
  void build(const QHash<QString, int> &nameToIndex);

  // The weights for a viseme. Empty for "sil" and for unknown names.
  QVector<Weight> weightsFor(const QString &viseme) const;

  // Number of morph targets the map addresses. This is the size of
  // the weight buffer the renderer needs.
  int targetCount() const { return m_targetCount; }

  // The set of viseme names this map knows.
  QStringList knownVisemes() const;

private:
  QHash<QString, QVector<Weight>> m_table;
  int m_targetCount = 0;
};