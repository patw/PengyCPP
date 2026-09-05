#ifndef PENGYCLI_SANITIZE_H
#define PENGYCLI_SANITIZE_H

#include <QString>
#include <QChar>

// Strip ANSI escape sequences (CSI, OSC, DCS, single-byte) and non-\n/\t C0
// control chars from untrusted text destined for the terminal. Stops injected
// tool/compiler output from moving the cursor or corrupting the line layout.
// Display-only: the raw bytes sent to the model are unchanged.
inline QString sanitizeDisplay(const QString& s) {
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    int i = 0;
    auto isCsiFinal = [](ushort cu) { return cu >= 0x40 && cu <= 0x7e; };
    while (i < n) {
        const ushort cu = s.at(i).unicode();
        if (cu == 0x1b) {
            if (i + 1 < n) {
                const ushort nx = s.at(i + 1).unicode();
                if (nx == QLatin1Char('[')) {                      // CSI
                    i += 2;
                    while (i < n) { ushort c = s.at(i).unicode(); ++i; if (isCsiFinal(c)) break; }
                } else if (nx == ']' || nx == 'P' || nx == '_' || nx == '^' || nx == 'X') {
                    i += 2;                                        // OSC/DCS/APC/PM/SOS
                    while (i < n) {
                        ushort c = s.at(i).unicode();
                        ++i;
                        if (c == 0x07) break;                                        // BEL ends OSC
                        if (c == 0x1b && i < n && s.at(i).unicode() == '\\') { ++i; break; }  // ST
                    }
                } else {
                    i += 2;                                        // single-byte escape
                }
            } else {
                ++i;
            }
            continue;
        }
        if (cu == '\n' || cu == '\t') { out.append(QChar(cu)); ++i; }
        else if (cu < 0x20 || cu == 0x7f) { ++i; }
        else { out.append(QChar(cu)); ++i; }
    }
    return out;
}

#endif // PENGYCLI_SANITIZE_H
