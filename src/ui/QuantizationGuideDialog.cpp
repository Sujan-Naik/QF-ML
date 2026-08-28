#include "ui/QuantizationGuideDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QTextBrowser>
#include <QVBoxLayout>


QuantizationGuideDialog::QuantizationGuideDialog(
    QWidget *parent
)
    : QDialog(parent) {
    setWindowTitle(
        QStringLiteral(
            "Talos — Quantization Guide"
        )
    );

    resize(
        900,
        760
    );

    auto *layout =
            new QVBoxLayout(
                this
            );

    auto *intro =
            new QLabel(
                this
            );

    intro->setWordWrap(
        true
    );

    intro->setText(
        QStringLiteral(
            "<b>What is quantization?</b><br>"
            "<br>"
            "Quantization stores model weights using fewer bits than "
            "full-precision formats. Lower-bit formats use less storage "
            "and usually require less VRAM, but can reduce model quality. "
            "Higher-bit formats generally preserve more of the original "
            "model quality at the cost of memory.<br><br>"
            "The names below are llama.cpp's technical quantization names. "
            "They are useful when choosing a GGUF, but you normally do not "
            "need to memorize them."
        )
    );

    layout->addWidget(
        intro
    );

    auto *browser =
            new QTextBrowser(
                this
            );

    browser->setOpenExternalLinks(
        false
    );

    browser->setHtml(
        QStringLiteral(
            R"(
<style>
body {
    font-family: sans-serif;
    font-size: 14px;
}
h2 {
    margin-top: 18px;
}
h3 {
    margin-top: 14px;
}
table {
    border-collapse: collapse;
    width: 100%;
}
th, td {
    border: 1px solid #888;
    padding: 7px;
    text-align: left;
    vertical-align: top;
}
th {
    font-weight: bold;
}
.small {
    color: #666;
}
.good {
    font-weight: bold;
}
</style>

<h2>Quick recommendation</h2>

<table>
<tr>
    <th>What you have</th>
    <th>Good starting point</th>
    <th>Why</th>
</tr>

<tr>
    <td>Very limited VRAM</td>
    <td><b>IQ2 / IQ3</b></td>
    <td>
        Fits models into smaller memory budgets, but quality can fall
        noticeably. Prefer a smaller model at a better quantization
        when possible.
    </td>
</tr>

<tr>
    <td>Typical consumer GPU</td>
    <td><b>Q4_K_M</b></td>
    <td>
        Excellent size/quality compromise and a very common default
        choice.
    </td>
</tr>

<tr>
    <td>More VRAM available</td>
    <td><b>Q5_K_M / Q6_K</b></td>
    <td>
        Spend additional memory to preserve more model quality.
    </td>
</tr>

<tr>
    <td>Lots of VRAM</td>
    <td><b>Q8_0 / F16</b></td>
    <td>
        Prioritize fidelity over memory usage.
    </td>
</tr>
</table>

<h2>The ordinary Q family</h2>

<table>
<tr>
    <th>Name</th>
    <th>Approx. bits/weight</th>
    <th>Human description</th>
    <th>Practical meaning</th>
</tr>

<tr>
    <td><b>Q2_K</b></td>
    <td>~2.56</td>
    <td>Very low compression</td>
    <td>
        Small model footprint, substantial quality tradeoff.
    </td>
</tr>

<tr>
    <td><b>Q3_K_S</b></td>
    <td>~3.44</td>
    <td>Low compression</td>
    <td>
        Very memory efficient, but generally below the quality of
        the common 4-bit choices.
    </td>
</tr>

<tr>
    <td><b>Q3_K_M</b></td>
    <td>~3.74</td>
    <td>Low compression — balanced</td>
    <td>
        A stronger 3-bit option when 4-bit does not fit.
    </td>
</tr>

<tr>
    <td><b>Q3_K_L</b></td>
    <td>~4.03</td>
    <td>Low compression — larger</td>
    <td>
        More memory than the smaller 3-bit variants for somewhat
        better quality.
    </td>
</tr>

<tr>
    <td><b>Q4_K_S</b></td>
    <td>~4.37</td>
    <td>4-bit — smaller</td>
    <td>
        Good compact 4-bit option.
    </td>
</tr>

<tr>
    <td><b>Q4_K_M</b></td>
    <td>~4.50</td>
    <td><b>4-bit — balanced</b></td>
    <td>
        The most useful general-purpose starting point for many models.
    </td>
</tr>

<tr>
    <td><b>Q5_K_S</b></td>
    <td>~5.21</td>
    <td>5-bit — smaller</td>
    <td>
        More quality than 4-bit at a moderate memory increase.
    </td>
</tr>

<tr>
    <td><b>Q5_K_M</b></td>
    <td>~5.50</td>
    <td><b>5-bit — high quality</b></td>
    <td>
        Strong choice when you have the VRAM for it.
    </td>
</tr>

<tr>
    <td><b>Q6_K</b></td>
    <td>~6.56</td>
    <td><b>6-bit — very high quality</b></td>
    <td>
        A relatively large model with little quantization damage.
    </td>
</tr>

<tr>
    <td><b>Q8_0</b></td>
    <td>8</td>
    <td>8-bit</td>
    <td>
        Very close to higher precision for many use cases, but requires
        substantially more memory.
    </td>
</tr>
</table>

<h2>IQ: Importance-aware quantization</h2>

<p>
The <b>IQ</b> family is different from the ordinary Q family.
These formats use importance-aware / importance-matrix based schemes.
They are particularly useful when trying to push a model into a very
tight memory budget.
</p>

<table>
<tr>
    <th>Name</th>
    <th>Approx. bits/weight</th>
    <th>Human description</th>
</tr>

<tr>
    <td><b>IQ1_S</b></td>
    <td>~1.50</td>
    <td>Extreme compression</td>
</tr>

<tr>
    <td><b>IQ1_M</b></td>
    <td>~1.75</td>
    <td>Extreme compression — slightly larger</td>
</tr>

<tr>
    <td><b>IQ2_XXS</b></td>
    <td>~2.06</td>
    <td>Ultra-low memory</td>
</tr>

<tr>
    <td><b>IQ2_XS</b></td>
    <td>~2.31</td>
    <td>Very low memory</td>
</tr>

<tr>
    <td><b>IQ2_S</b></td>
    <td>~2.50</td>
    <td>Very low memory — improved quality</td>
</tr>

<tr>
    <td><b>IQ2_M</b></td>
    <td>~2.70</td>
    <td>Very low memory — stronger 2-bit option</td>
</tr>

<tr>
    <td><b>IQ3_XXS</b></td>
    <td>~3.06</td>
    <td>Low memory</td>
</tr>

<tr>
    <td><b>IQ3_S</b></td>
    <td>~3.44</td>
    <td>Low memory — stronger 3-bit option</td>
</tr>

<tr>
    <td><b>IQ3_M</b></td>
    <td>~3.66</td>
    <td>Low memory — stronger 3-bit quality</td>
</tr>

<tr>
    <td><b>IQ4_XS</b></td>
    <td>~4.25</td>
    <td>4-bit importance-aware</td>
</tr>

<tr>
    <td><b>IQ4_NL</b></td>
    <td>~4.50</td>
    <td>4-bit non-linear</td>
</tr>
</table>

<h2>What do the suffixes mean?</h2>

<p>
The suffixes such as <b>S</b>, <b>M</b>, <b>L</b>, <b>XS</b>,
<b>XXS</b> describe different encoding / packing variants.
They are <b>not simply a universal quality ranking</b>.
</p>

<p>
For normal users, the useful rule is:
</p>

<ul>
<li>
    <b>Don't choose purely from the suffix.</b>
    Compare the actual model, quantization, file size and available VRAM.
</li>
<li>
    <b>Q4_K_M</b> and <b>Q5_K_M</b> are easy default choices for many models.
</li>
<li>
    <b>IQ</b> formats are useful when memory is especially constrained.
</li>
<li>
    Moving from a 2-bit model to a smaller model at 4-bit can often be
    a better practical choice than squeezing a huge model into 2-bit.
</li>
</ul>

<h2>F16 / BF16 / F32</h2>

<table>
<tr>
    <th>Format</th>
    <th>Bits</th>
    <th>Description</th>
</tr>

<tr>
    <td><b>F32</b></td>
    <td>32</td>
    <td>
        Full 32-bit floating point. Very large memory footprint.
    </td>
</tr>

<tr>
    <td><b>F16</b></td>
    <td>16</td>
    <td>
        Half precision floating point. Much larger than typical GGUF
        quantizations.
    </td>
</tr>

<tr>
    <td><b>BF16</b></td>
    <td>16</td>
    <td>
        Brain floating point. Useful on hardware with strong BF16 support.
    </td>
</tr>
</table>

<h2>How to choose for Talos</h2>

<p>
Talos should normally choose by <b>model size first</b> and
<b>quantization second</b>.
</p>

<p>
For example, suppose you have 12 GB of usable VRAM:
</p>

<ul>
<li>
    A huge model at IQ2 may technically fit, but may produce worse
    results than a smaller model at Q4_K_M.
</li>
<li>
    A medium model at Q4_K_M is often a much more sensible starting point.
</li>
<li>
    If a Q5_K_M version fits comfortably, it is worth considering.
</li>
</ul>

<p>
The model browser therefore shows both:
</p>

<ul>
<li>
    the <b>quantization</b>
</li>
<li>
    the <b>approximate model / VRAM requirement</b>
</li>
</ul>

<p class="small">
The bit-per-weight values and quantization terminology here follow the
llama.cpp quantization/type definitions. Actual runtime memory usage can
be higher than the raw model size because of KV cache, context length,
batching and backend/runtime buffers.
</p>
)"
        )
    );

    layout->addWidget(
        browser,
        1
    );

    auto *buttons =
            new QDialogButtonBox(
                QDialogButtonBox::Close,
                this
            );

    connect(
        buttons,
        &QDialogButtonBox::rejected,
        this,
        &QDialog::reject
    );

    layout->addWidget(
        buttons
    );
}
