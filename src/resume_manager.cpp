#include "resume_manager.h"
#include "terminal/buffer.h"
#include <algorithm>
#include <cctype>

namespace {

std::wstring BufferRowText(const TerminalBuffer& buffer, int row) {
    std::wstring text;
    if (row < 0 || row >= buffer.GetRows())
        return text;
    int cols = buffer.GetCols();
    text.reserve(cols);
    for (int c = 0; c < cols; c++) {
        const Cell& cell = buffer.At(row, c);
        if (cell.width == 0)
            continue;
        text += (cell.ch == 0) ? L' ' : cell.ch;
        if (cell.ch2 != 0)
            text += cell.ch2;
    }
    return text;
}

std::wstring TrimW(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && iswspace(s[start])) start++;
    size_t end = s.size();
    while (end > start && iswspace(s[end - 1])) end--;
    return s.substr(start, end - start);
}

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring result = s;
    for (auto& c : result)
        c = towlower(c);
    return result;
}

const wchar_t* kAgentNames[] = { L"claude", L"gemini", L"codex" };
constexpr int kAgentCount = 3;

}

std::wstring ResumeManager::GetResumeIniPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(exePath) + L"resume.ini";
}

std::wstring ResumeManager::NormalizeDir(const std::wstring& dir) {
    std::wstring norm = dir;
    for (auto& c : norm) {
        if (c == L'\\') c = L'/';
    }
    // Remove trailing slash
    while (norm.size() > 1 && norm.back() == L'/')
        norm.pop_back();
    return ToLowerW(norm);
}

void ResumeManager::SaveResume(const std::wstring& dir, const std::wstring& agentName,
                                const std::wstring& resumeCmd) {
    std::wstring path = GetResumeIniPath();
    std::wstring section = NormalizeDir(dir);
    WritePrivateProfileStringW(section.c_str(), agentName.c_str(),
                               resumeCmd.c_str(), path.c_str());
}

bool ResumeManager::LoadResume(const std::wstring& dir, const std::wstring& agentName,
                                ResumeEntry& out) {
    std::wstring path = GetResumeIniPath();
    std::wstring section = NormalizeDir(dir);
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section.c_str(), agentName.c_str(), L"",
                             buf, 1024, path.c_str());
    if (buf[0] == L'\0')
        return false;
    out.agentName = agentName;
    out.resumeCmd = buf;
    out.directory = dir;
    return true;
}

void ResumeManager::ClearResume(const std::wstring& dir, const std::wstring& agentName) {
    std::wstring path = GetResumeIniPath();
    std::wstring section = NormalizeDir(dir);
    WritePrivateProfileStringW(section.c_str(), agentName.c_str(),
                               nullptr, path.c_str());
}

bool IsResumeIdChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') ||
           (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') ||
           c == L'-' || c == L'_';
}

// Extract resume ID starting at pos, skipping quotes and whitespace
std::wstring ExtractResumeId(const std::wstring& line, size_t pos) {
    while (pos < line.size() && (iswspace(line[pos]) || line[pos] == L'\'' || line[pos] == L'"'))
        pos++;
    if (pos >= line.size()) return L"";

    size_t start = pos;
    while (pos < line.size() && IsResumeIdChar(line[pos])) pos++;
    if (pos <= start) return L"";
    return line.substr(start, pos - start);
}

// Check if a character is plain ASCII text (not box-drawing, not control)
bool IsPlainChar(wchar_t c) {
    return c >= 0x20 && c < 0x2500;
}

// Strip box-drawing/decorative characters, keep only plain text
std::wstring StripDecoration(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (IsPlainChar(c))
            out += c;
    }
    return out;
}

bool ResumeManager::ScanForResumeCommand(const TerminalBuffer& buffer,
                                          std::wstring& agentName,
                                          std::wstring& resumeCmd) {
    int cursorRow = buffer.GetCursorRow();
    int startRow = (std::max)(0, cursorRow - 10);
    int endRow = (std::min)(buffer.GetRows() - 1, cursorRow);

    for (int row = endRow; row >= startRow; row--) {
        std::wstring rawLine = BufferRowText(buffer, row);
        std::wstring line = StripDecoration(rawLine);
        std::wstring lower = ToLowerW(line);

        if (lower.find(L"resume") == std::wstring::npos)
            continue;

        // Find agent name first, then look for "resume" AFTER it
        for (int i = 0; i < kAgentCount; i++) {
            std::wstring agent = kAgentNames[i];
            size_t agentPos = lower.find(agent);
            if (agentPos == std::wstring::npos)
                continue;

            size_t searchFrom = agentPos + agent.size();
            size_t rpos = lower.find(L"resume", searchFrom);
            if (rpos == std::wstring::npos)
                continue;

            size_t idStartPos = rpos + 6;
            std::wstring id = ExtractResumeId(line, idStartPos);
            if (id.size() < 4)
                continue;

            bool hasDashes = (rpos >= 2 && line[rpos - 1] == L'-' && line[rpos - 2] == L'-');

            std::wstring between = TrimW(line.substr(searchFrom, rpos - searchFrom));
            if (hasDashes) {
                while (!between.empty() && between.back() == L'-')
                    between.pop_back();
                between = TrimW(between);
            }

            std::wstring cmd = agent;
            if (!between.empty())
                cmd += L" " + between;
            cmd += hasDashes ? L" --resume " : L" resume ";
            cmd += id;

            agentName = agent;
            resumeCmd = cmd;
            return true;
        }
    }
    return false;
}

bool ResumeManager::DetectAgentLaunch(const std::wstring& commandLine,
                                       std::wstring& agentName) {
    std::wstring trimmed = TrimW(commandLine);
    if (trimmed.empty())
        return false;

    std::wstring lower = ToLowerW(trimmed);

    // Already contains resume keyword, skip
    if (lower.find(L"--resume") != std::wstring::npos ||
        lower.find(L" resume ") != std::wstring::npos)
        return false;

    // Extract first token
    size_t spacePos = trimmed.find(L' ');
    std::wstring firstToken = (spacePos != std::wstring::npos)
        ? ToLowerW(trimmed.substr(0, spacePos)) : ToLowerW(trimmed);

    for (int i = 0; i < kAgentCount; i++) {
        if (firstToken == kAgentNames[i]) {
            agentName = kAgentNames[i];
            return true;
        }
    }
    return false;
}
