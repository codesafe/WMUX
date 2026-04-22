#pragma once
#include "pane/pane_session.h"
#include <memory>
#include <string>

std::unique_ptr<IPaneSession> CreatePaneSession();
std::unique_ptr<IPaneSession> CreateDetachablePaneSession();
std::unique_ptr<IPaneSession> AttachPaneSession(const std::wstring& sessionId);
std::wstring LaunchDetachedSessionHost(int cols, int rows,
                                       const std::wstring& shell,
                                       const std::wstring& workingDir);
