#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

class TerminalBuffer;

class ResumeManager {
public:
    struct ResumeEntry {
        std::wstring agentName;
        std::wstring resumeCmd;
        std::wstring directory;
    };

    static std::wstring GetResumeIniPath();

    void SaveResume(const std::wstring& dir, const std::wstring& agentName,
                    const std::wstring& resumeCmd);
    bool LoadResume(const std::wstring& dir, const std::wstring& agentName,
                    ResumeEntry& out);
    void ClearResume(const std::wstring& dir, const std::wstring& agentName);

    static bool ScanForResumeCommand(const TerminalBuffer& buffer,
                                     std::wstring& agentName,
                                     std::wstring& resumeCmd);

    static bool DetectAgentLaunch(const std::wstring& commandLine,
                                  std::wstring& agentName);

private:
    static std::wstring NormalizeDir(const std::wstring& dir);
};
