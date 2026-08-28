#pragma once

#include <QString>
#include <vector>

class ITranscriber {
public:
    virtual ~ITranscriber() = default;

    virtual bool isLoaded() const = 0;

    virtual QString transcribe(
        const std::vector<float> &pcm32f
    ) = 0;
};
