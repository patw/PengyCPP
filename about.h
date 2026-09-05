#pragma once
// Shared About info — edition name, links, and blurb for all three frontends
// (GUI, CLI, Web). Keep the description/links here in sync with the Python
// and Rust editions' own About screens.
#include "version.h"
#include <QString>
#include <QDate>

inline const QString kPengyGithubUrl   = "https://github.com/patw/PengyCPP";
inline const QString kPengyWebsiteUrl  = "https://pengy.catbee.ca";
inline const QString kPengyLicenseUrl  = kPengyGithubUrl + "/blob/main/LICENSE";
inline const QString kPengyLicenseName = "MIT License";

inline const QString kPengyDescription =
    "Pengy is a local-first AI agent that connects to any OpenAI-compatible API "
    "(OpenAI, Ollama, vLLM, Groq, OpenRouter, or a local endpoint) and gives the "
    "model tools to operate on your filesystem, run code, search the web, and "
    "more — all with your approval.";

inline const QString kCatbeeUrl = "https://catbee.ca";
inline const QString kCatbeeBlurb =
    "Pengy is part of Catbee — a collection of open-source, self-hosted AI tools "
    "for hyper-personal computing, designed to be self-hosted, fully controllable, "
    "and yours to own.";

// The year Pengy was first published — kept in sync with LICENSE's copyright year.
inline constexpr int kPengyFoundingYear = 2026;

// e.g. pengyEditionLine("C++") -> "Pengy C++ - 1.8.1"
inline QString pengyEditionLine(const QString& edition) {
    return QStringLiteral("Pengy %1 - %2").arg(edition, QStringLiteral(PENGY_VERSION));
}

// e.g. "Copyright © 2026 Pat Wendorf (dungeons@gmail.com)", ranged once the
// year rolls over past kPengyFoundingYear.
inline QString pengyCopyrightLine() {
    int year = QDate::currentDate().year();
    QString yearStr = year <= kPengyFoundingYear
        ? QString::number(kPengyFoundingYear)
        : QStringLiteral("%1–%2").arg(kPengyFoundingYear).arg(year);
    return QStringLiteral("Copyright © %1 Pat Wendorf (dungeons@gmail.com)").arg(yearStr);
}
