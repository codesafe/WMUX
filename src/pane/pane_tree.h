#pragma once
#include "pane/pane.h"
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>
#include <d2d1.h>

enum class SplitDirection { Horizontal, Vertical };

struct SplitNode {
    D2D1_RECT_F rect = {0, 0, 0, 0};

    // Leaf: holds a pane
    std::unique_ptr<Pane> pane;
    uint32_t paneId = 0;

    // Branch: holds two children
    SplitDirection direction = SplitDirection::Vertical;
    float splitRatio = 0.5f;
    std::unique_ptr<SplitNode> first;
    std::unique_ptr<SplitNode> second;
    SplitNode* parent = nullptr;

    bool IsLeaf() const { return pane != nullptr; }
};

class PaneManager {
public:
    struct SeparatorLine {
        float x1, y1, x2, y2;
    };

    PaneManager() = default;

    bool Initialize(D2D1_RECT_F clientRect, HWND hwnd, UINT ptyMsg,
                    float cellWidth, float cellHeight,
                    const std::wstring& workingDir = L"");

    bool SplitActive(SplitDirection dir, HWND hwnd, UINT ptyMsg,
                     float cellWidth, float cellHeight,
                     const std::wstring& workingDir = L"");
    bool CloseActive();
    bool ClosePaneById(uint32_t id);

    void MoveFocus(SplitDirection dir, bool forward);
    void ToggleZoom();
    bool IsZoomed() const { return m_zoomed; }
    bool HasSinglePane() const { return m_root && m_root->IsLeaf(); }

    // Alt+Arrow: Move pane in direction (tree restructuring)
    void MovePane(SplitDirection dir, bool forward);
    // Alt+Shift+Arrow: Swap pane content with neighbor
    void SwapPaneContent(SplitDirection dir, bool forward);

    void Relayout(D2D1_RECT_F clientRect, float cellWidth, float cellHeight);

    Pane* FindPaneById(uint32_t id);
    Pane* GetActivePane();
    SplitNode* GetActiveNode() { return m_activeNode; }
    void SetActivePane(Pane* pane);

    void ForEachLeaf(std::function<void(SplitNode& node)> fn);
    void CollectSeparators(std::vector<SeparatorLine>& lines);
    Pane* FindPaneAtPoint(float x, float y);
    bool FindPaneAndRectAtPoint(float x, float y, Pane*& outPane, D2D1_RECT_F& outRect);

    // Separator drag support
    SplitNode* FindSeparatorAtPoint(float x, float y);
    void UpdateSplitRatio(SplitNode* node, float x, float y, float cellWidth, float cellHeight);

private:
    void LayoutNode(SplitNode* node, D2D1_RECT_F rect,
                    float cellWidth, float cellHeight);
    SplitNode* FindLeafInDirection(SplitNode* from,
                                    SplitDirection dir, bool forward);
    void CollectSeparatorsRecursive(SplitNode* node,
                                     std::vector<SeparatorLine>& lines);
    void ForEachLeafRecursive(SplitNode* node,
                               std::function<void(SplitNode&)>& fn);
    Pane* FindPaneByIdRecursive(SplitNode* node, uint32_t id);

    // Helper: Find physical neighbor in screen direction
    SplitNode* FindPhysicalNeighbor(SplitNode* from, SplitDirection dir, bool forward);
    // Helper: Calculate overlap between two rects
    float CalculateOverlap(D2D1_RECT_F r1, D2D1_RECT_F r2, SplitDirection dir);

    std::unique_ptr<SplitNode> m_root;
    SplitNode* m_activeNode = nullptr;
    bool m_zoomed = false;
    D2D1_RECT_F m_fullRect = {0, 0, 0, 0};

    uint32_t m_nextPaneId = 1;
    static constexpr float SEP_WIDTH = 2.0f;
    static constexpr int MIN_COLS = 4;
    static constexpr int MIN_ROWS = 2;
};
