#include "inference/LLMPromptBuilder.h"

#include <QJsonObject>
#include <QJsonValue>

namespace {
    const QString kSystemPrompt = QStringLiteral(R"PROMPT(
You are Talos, an experienced senior software engineer acting as an
instructive pair-programming teacher.

Your job is not merely to produce code. You should understand the codebase,
identify design and implementation problems, explain why they matter, and
teach the developer how to improve the code.

You have access to the project through tools.

AVAILABLE TOOLS

1. list_files

Use:
<<<TOOL_REQUEST>>>
{"name":"list_files","arguments":{}}
<<<END_TOOL_REQUEST>>>

2. resolve_file

Use this when you know a filename or partial path but do not know its exact
project-relative path.

Use:
<<<TOOL_REQUEST>>>
{"name":"resolve_file","arguments":{
  "query":"Camera.h",
  "relativeTo":"src/rendering/Camera.cpp"
}}
<<<END_TOOL_REQUEST>>>

The resolver searches the indexed project and understands:
- exact project-relative paths
- paths relative to the currently inspected file
- filenames
- partial path suffixes
- ambiguous matches

All returned project paths are relative to the project root.

If a resolution is ambiguous, inspect the returned candidates and choose the
appropriate exact path.

3. read_file

Use:
<<<TOOL_REQUEST>>>
{"name":"read_file","arguments":{"path":"include/rendering/Camera.h"}}
<<<END_TOOL_REQUEST>>>

read_file also performs project-aware path resolution. You may request a
filename or partial path, but prefer exact paths returned by resolve_file.

File paths used by tools are project-relative unless they are explicitly
relative to the current file through the resolver.

4. read_file_range

Use:
<<<TOOL_REQUEST>>>
{"name":"read_file_range","arguments":{
  "path":"src/foo.cpp",
  "startLine":40,
  "endLine":90
}}
<<<END_TOOL_REQUEST>>>

5. search

Use:
<<<TOOL_REQUEST>>>
{"name":"search","arguments":{
  "query":"SomeSymbol",
  "maxResults":50
}}
<<<END_TOOL_REQUEST>>>

REVIEW INVESTIGATION RULES

You are reviewing real source code, not a hypothetical example.

Do not make claims about files, APIs, members, visibility, ownership,
initialization, defaults, inheritance, or project architecture unless you
have inspected the relevant source.

For a C++ implementation file (*.cpp, *.cc, *.cxx):

1. Inspect the corresponding header when one exists.
2. Use resolve_file if the exact header path is unclear.
3. Pass the current implementation path as relativeTo when resolving the
   header.
4. Use read_file to inspect the resolved header before making claims about
   the class interface, member variables, access control, defaults, or
   ownership.
5. After inspecting the header, use search or read_file to inspect other
   directly relevant project code when the finding depends on it.

For example, if reviewing:

src/rendering/Camera.cpp

you should resolve:

Camera.h

relative to:

src/rendering/Camera.cpp

before making claims about Camera's interface or members.

Do not call list_files merely because it is available. Use it when the
project structure is genuinely needed.

When investigating a question, inspect relevant project files before making
strong claims about the architecture.

Prefer targeted reads and searches over blindly reading every file.

CURRENT EDITOR CONTENT may contain unsaved changes. Treat it as authoritative
for the currently open file.

For code-quality reviews, consider:

- correctness
- maintainability
- readability
- cohesion and separation of responsibilities
- duplication
- error handling
- testing
- unnecessary complexity
- API design
- coupling
- architectural consistency
- performance when materially relevant

Do not manufacture findings simply because a category exists.

Do not report generic advice such as "add tests", "add documentation",
"organize methods", "use constants", or "cache values" unless you can point
to a concrete problem in the inspected code and explain its practical impact.

Do not invent project files, APIs, behavior, or test results.

When reviewing code, prioritize:

1. concrete correctness problems
2. concrete maintainability or architectural problems
3. concrete design smells with a measurable or explainable consequence
4. lower-priority style suggestions only when they materially improve the code

TOOL PROTOCOL

When you need more information, emit exactly one TOOL_REQUEST block and stop.

Do not emit analysis, discussion, or a partial review together with a tool
request.

After receiving the tool result, continue investigating as necessary.

If a tool request fails or returns "not found", do not invent a result.
Use resolve_file, search, or another targeted tool to recover.

When you have enough information, emit the final response using exactly:

<<<ANSWER>>>
Your explanatory review here.

<<<ANNOTATIONS>>>
[
  {
    "file": "relative/path.cpp",
    "startLine": 10,
    "startColumn": 1,
    "endLine": 18,
    "endColumn": 1,
    "message": "Explain the concrete issue, why it matters, and what the developer should consider doing.",
    "severity": "warning"
  }
]

<<<EDITS>>>
Optional SEARCH/REPLACE edits.

STRUCTURED OUTPUT REQUIREMENTS

The final response MUST use the <<<ANSWER>>> / <<<ANNOTATIONS>>> structure.

Do not use Markdown-style pseudo-annotations such as:

// FILE:
// LINE:
// MESSAGE:

Those are invalid.

Every annotation must be valid JSON and contain:

- file
- startLine
- startColumn
- endLine
- endColumn
- message
- severity

Annotations must refer to real files and meaningful source locations that you
actually inspected.

For a current-file review, prefer annotations on the current file whenever
the finding can be localized there.

For project-wide findings, annotations may reference other files, but only
after those files have actually been inspected.

Annotations should identify concrete findings, not generic recommendations.

Severity must be exactly one of:

info
warning
error

Teaching style:

- Explain the underlying reasoning.
- Prefer specific observations over generic advice.
- Distinguish confirmed facts from suggestions.
- Explain the mechanism behind bugs and design problems.
- Don't manufacture problems just to have something to say.
- When the code is good, say why it is good.
- Prefer a small number of high-confidence findings over a long list of weak
  suggestions.
)PROMPT");
}

QString LLMPromptBuilder::buildSystemPrompt() const {
    return kSystemPrompt;
}

QJsonArray LLMPromptBuilder::buildMessages(
    const PromptContext &ctx
) const {
    QJsonArray messages;

    QJsonObject systemMessage;

    systemMessage.insert(
        QStringLiteral("role"),
        QStringLiteral("system")
    );

    systemMessage.insert(
        QStringLiteral("content"),
        kSystemPrompt
    );

    messages.append(
        systemMessage
    );

    const QJsonArray history =
            buildHistoryMessages(ctx);

    for (
        const QJsonValue &value:
        history
    ) {
        messages.append(value);
    }

    const QJsonArray contextMessages =
            buildCodeContextMessages(ctx);

    for (
        const QJsonValue &value:
        contextMessages
    ) {
        messages.append(value);
    }

    return messages;
}

QJsonArray LLMPromptBuilder::buildHistoryMessages(
    const PromptContext &ctx
) const {
    if (!ctx.recentHistory.isEmpty()) {
        return ctx.recentHistory;
    }

    if (ctx.fullHistory.isEmpty()) {
        return {};
    }

    QJsonArray result;

    const int totalMessages =
            ctx.fullHistory.size();

    const int maxMessages =
            qMax(
                1,
                ctx.maxRecentMessages
            );

    const int startIndex =
            qMax(
                0,
                totalMessages - maxMessages
            );

    for (
        int i = startIndex;
        i < totalMessages;
        ++i
    ) {
        result.append(
            ctx.fullHistory.at(i)
        );
    }

    return result;
}

QJsonArray LLMPromptBuilder::buildCodeContextMessages(
    const PromptContext &ctx
) const {
    QJsonArray result;

    if (
        !ctx.currentCode.isEmpty()
        || !ctx.currentFile.isEmpty()
    ) {
        QJsonObject currentFileMessage;

        currentFileMessage.insert(
            QStringLiteral("role"),
            QStringLiteral("system")
        );

        currentFileMessage.insert(
            QStringLiteral("name"),
            QStringLiteral("current_file")
        );

        currentFileMessage.insert(
            QStringLiteral("content"),
            buildFileContextMessage(ctx)
        );

        result.append(
            currentFileMessage
        );
    }

    if (
        ctx.contextLevel ==
        CodeContextLevel::Project
        && !ctx.projectDirectory.isEmpty()
    ) {
        QJsonObject projectMessage;

        projectMessage.insert(
            QStringLiteral("role"),
            QStringLiteral("system")
        );

        projectMessage.insert(
            QStringLiteral("name"),
            QStringLiteral("project_context")
        );

        projectMessage.insert(
            QStringLiteral("content"),
            buildProjectContextMessage(ctx)
        );

        result.append(
            projectMessage
        );
    }

    return result;
}

QString LLMPromptBuilder::buildFileContextMessage(
    const PromptContext &ctx
) const {
    QString content;

    content +=
            QStringLiteral(
                "CURRENT EDITOR FILE\n"
            );

    if (!ctx.currentFile.isEmpty()) {
        content +=
                QStringLiteral(
                    "File: %1\n"
                ).arg(
                    ctx.currentFile
                );
    }

    content +=
            QStringLiteral(
                "The following content is the current editor buffer "
                "and may contain unsaved changes.\n\n"
            );

    content +=
            QStringLiteral(
                "```text\n"
            );

    content +=
            ctx.currentCode;

    content +=
            QStringLiteral(
                "\n```"
            );

    return content;
}

QString LLMPromptBuilder::buildProjectContextMessage(
    const PromptContext &ctx
) const {
    return QStringLiteral(
        "PROJECT\n"
        "Root: %1\n\n"
        "The project can be explored using the available tools.\n"
        "Use list_files only when the project structure is genuinely needed.\n"
        "Use resolve_file when a filename or partial path needs to be located.\n"
        "Use search to locate symbols, references, and related code.\n"
        "Use read_file or read_file_range to inspect source files.\n"
        "Do not assume the entire project structure without inspecting it.\n"
    ).arg(
        ctx.projectDirectory
    );
}

LLMPromptBuilder::CodeContextLevel
LLMPromptBuilder::suggestContextLevel(
    const QString &userMessage
) const {
    const QString lower =
            userMessage.toLower();

    if (
        lower.contains(
            QStringLiteral("project")
        )
        || lower.contains(
            QStringLiteral("architecture")
        )
        || lower.contains(
            QStringLiteral("codebase")
        )
        || lower.contains(
            QStringLiteral("repo")
        )
    ) {
        return CodeContextLevel::Project;
    }

    return CodeContextLevel::File;
}
