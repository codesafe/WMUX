#include "pane/pane_tree.h"
#include <algorithm>

bool PaneManager::Initialize(D2D1_RECT_F clientRect, HWND hwnd, UINT ptyMsg,
                              float cellWidth, float cellHeight,
                              const std::wstring& workingDir) {
    m_fullRect = clientRect;
    m_root = std::make_unique<SplitNode>();
    m_root->paneId = m_nextPaneId++;
    m_root->pane = std::make_unique<Pane>();
    m_root->rect = clientRect;

    // Account for 4px padding on each side (8px total)
    constexpr float PADDING = 8.0f;
    float availW = (clientRect.right - clientRect.left) - PADDING;
    float availH = (clientRect.bottom - clientRect.top) - PADDING;
    int cols = (std::max)(1, static_cast<int>(availW / cellWidth));
    int rows = (std::max)(1, static_cast<int>(availH / cellHeight));

    if (!m_root->pane->Start(cols, rows, hwnd, ptyMsg, L"", m_root->paneId, workingDir))
        return false;

    m_activeNode = m_root.get();
    return true;
}

bool PaneManager::SplitActive(SplitDirection dir, HWND hwnd, UINT ptyMsg,
                               float cellWidth, float cellHeight,
                               const std::wstring& workingDir) {
    if (!m_activeNode || !m_activeNode->IsLeaf() || m_zoomed)
        return false;

    // Check minimum size
    D2D1_RECT_F parentRect = m_activeNode->rect;
    float availW = parentRect.right - parentRect.left;
    float availH = parentRect.bottom - parentRect.top;

    if (dir == SplitDirection::Vertical) {
        float half = (availW - SEP_WIDTH) / 2.0f;
        if (static_cast<int>(half / cellWidth) < MIN_COLS) return false;
    } else {
        float half = (availH - SEP_WIDTH) / 2.0f;
        if (static_cast<int>(half / cellHeight) < MIN_ROWS) return false;
    }

    SplitNode* oldLeaf = m_activeNode;

    // Build new branch
    auto newBranch = std::make_unique<SplitNode>();
    newBranch->direction = dir;
    newBranch->splitRatio = 0.5f;
    newBranch->rect = parentRect;
    newBranch->parent = oldLeaf->parent;

    // First child: move existing pane
    newBranch->first = std::make_unique<SplitNode>();
    newBranch->first->pane = std::move(oldLeaf->pane);
    newBranch->first->paneId = oldLeaf->paneId;
    newBranch->first->parent = newBranch.get();

    // Second child: new pane
    newBranch->second = std::make_unique<SplitNode>();
    newBranch->second->paneId = m_nextPaneId++;
    newBranch->second->pane = std::make_unique<Pane>();
    newBranch->second->parent = newBranch.get();

    // Start new pane (size will be corrected by Relayout)
    if (!newBranch->second->pane->Start(10, 5, hwnd, ptyMsg, L"",
                                         newBranch->second->paneId, workingDir)) {
        // Restore old pane
        oldLeaf->pane = std::move(newBranch->first->pane);
        return false;
    }

    // Replace in tree
    SplitNode* parent = oldLeaf->parent;
    SplitNode* branchPtr = newBranch.get();

    if (!parent) {
        m_root = std::move(newBranch);
    } else if (parent->first.get() == oldLeaf) {
        parent->first = std::move(newBranch);
    } else {
        parent->second = std::move(newBranch);
    }

    m_activeNode = branchPtr->second.get();

    LayoutNode(m_root.get(), m_fullRect, cellWidth, cellHeight);
    return true;
}

bool PaneManager::CloseActive() {
    if (!m_activeNode) return false;

    if (m_zoomed) {
        m_zoomed = false;
    }

    m_activeNode->pane->Stop();

    SplitNode* parent = m_activeNode->parent;
    if (!parent) {
        // Last pane
        return false;
    }

    // Find sibling
    std::unique_ptr<SplitNode> sibling;
    if (parent->first.get() == m_activeNode) {
        sibling = std::move(parent->second);
    } else {
        sibling = std::move(parent->first);
    }

    sibling->parent = parent->parent;
    SplitNode* siblingPtr = sibling.get();

    if (!parent->parent) {
        m_root = std::move(sibling);
    } else if (parent->parent->first.get() == parent) {
        parent->parent->first = std::move(sibling);
    } else {
        parent->parent->second = std::move(sibling);
    }

    // Focus first leaf in sibling subtree
    m_activeNode = siblingPtr;
    while (!m_activeNode->IsLeaf())
        m_activeNode = m_activeNode->first.get();

    return true;
}

bool PaneManager::ClosePaneById(uint32_t id) {
    // Find the node with this pane ID and make it active, then close
    SplitNode* target = nullptr;
    ForEachLeaf([&](SplitNode& node) {
        if (node.paneId == id)
            target = &node;
    });
    if (!target) return true; // already gone
    m_activeNode = target;
    return CloseActive();
}

void PaneManager::MoveFocus(SplitDirection dir, bool forward) {
    SplitNode* found = FindLeafInDirection(m_activeNode, dir, forward);
    if (found)
        m_activeNode = found;
}

void PaneManager::ToggleZoom() {
    m_zoomed = !m_zoomed;
}

void PaneManager::Relayout(D2D1_RECT_F clientRect, float cellWidth, float cellHeight) {
    m_fullRect = clientRect;
    if (m_root)
        LayoutNode(m_root.get(), clientRect, cellWidth, cellHeight);
}

void PaneManager::LayoutNode(SplitNode* node, D2D1_RECT_F rect,
                              float cellWidth, float cellHeight) {
    node->rect = rect;

    if (node->IsLeaf()) {
        // Account for 4px padding on each side (8px total)
        constexpr float PADDING = 8.0f;
        float availW = (rect.right - rect.left) - PADDING;
        float availH = (rect.bottom - rect.top) - PADDING;
        int cols = (std::max)(1, static_cast<int>(availW / cellWidth));
        int rows = (std::max)(1, static_cast<int>(availH / cellHeight));
        node->pane->Resize(cols, rows);
        return;
    }

    float totalW = rect.right - rect.left;
    float totalH = rect.bottom - rect.top;

    if (node->direction == SplitDirection::Vertical) {
        float firstW = (totalW - SEP_WIDTH) * node->splitRatio;
        firstW = (std::max)(cellWidth, (std::min)(firstW, totalW - SEP_WIDTH - cellWidth));

        D2D1_RECT_F r1 = {rect.left, rect.top, rect.left + firstW, rect.bottom};
        D2D1_RECT_F r2 = {rect.left + firstW + SEP_WIDTH, rect.top, rect.right, rect.bottom};

        LayoutNode(node->first.get(), r1, cellWidth, cellHeight);
        LayoutNode(node->second.get(), r2, cellWidth, cellHeight);
    } else {
        float firstH = (totalH - SEP_WIDTH) * node->splitRatio;
        firstH = (std::max)(cellHeight, (std::min)(firstH, totalH - SEP_WIDTH - cellHeight));

        D2D1_RECT_F r1 = {rect.left, rect.top, rect.right, rect.top + firstH};
        D2D1_RECT_F r2 = {rect.left, rect.top + firstH + SEP_WIDTH, rect.right, rect.bottom};

        LayoutNode(node->first.get(), r1, cellWidth, cellHeight);
        LayoutNode(node->second.get(), r2, cellWidth, cellHeight);
    }
}

Pane* PaneManager::FindPaneById(uint32_t id) {
    return FindPaneByIdRecursive(m_root.get(), id);
}

Pane* PaneManager::FindPaneByIdRecursive(SplitNode* node, uint32_t id) {
    if (!node) return nullptr;
    if (node->IsLeaf()) {
        return (node->paneId == id) ? node->pane.get() : nullptr;
    }
    Pane* p = FindPaneByIdRecursive(node->first.get(), id);
    if (p) return p;
    return FindPaneByIdRecursive(node->second.get(), id);
}

Pane* PaneManager::GetActivePane() {
    return m_activeNode ? m_activeNode->pane.get() : nullptr;
}

void PaneManager::SetActivePane(Pane* pane) {
    if (!pane) return;
    ForEachLeaf([&](SplitNode& node) {
        if (node.pane.get() == pane) {
            m_activeNode = &node;
        }
    });
}

void PaneManager::ForEachLeaf(std::function<void(SplitNode& node)> fn) {
    ForEachLeafRecursive(m_root.get(), fn);
}

void PaneManager::ForEachLeafRecursive(SplitNode* node,
                                        std::function<void(SplitNode&)>& fn) {
    if (!node) return;
    if (node->IsLeaf()) { fn(*node); return; }
    ForEachLeafRecursive(node->first.get(), fn);
    ForEachLeafRecursive(node->second.get(), fn);
}

void PaneManager::CollectSeparators(std::vector<SeparatorLine>& lines) {
    lines.clear();
    CollectSeparatorsRecursive(m_root.get(), lines);
}

void PaneManager::CollectSeparatorsRecursive(SplitNode* node,
                                              std::vector<SeparatorLine>& lines) {
    if (!node || node->IsLeaf()) return;

    if (node->direction == SplitDirection::Vertical) {
        float x = node->first->rect.right;
        lines.push_back({x, node->rect.top, x + SEP_WIDTH, node->rect.bottom});
    } else {
        float y = node->first->rect.bottom;
        lines.push_back({node->rect.left, y, node->rect.right, y + SEP_WIDTH});
    }

    CollectSeparatorsRecursive(node->first.get(), lines);
    CollectSeparatorsRecursive(node->second.get(), lines);
}

Pane* PaneManager::FindPaneAtPoint(float x, float y) {
    Pane* result = nullptr;
    ForEachLeaf([&](SplitNode& node) {
        if (x >= node.rect.left && x < node.rect.right &&
            y >= node.rect.top && y < node.rect.bottom) {
            result = node.pane.get();
        }
    });
    return result;
}

bool PaneManager::FindPaneAndRectAtPoint(float x, float y,
                                          Pane*& outPane, D2D1_RECT_F& outRect) {
    bool found = false;
    ForEachLeaf([&](SplitNode& node) {
        if (!found && x >= node.rect.left && x < node.rect.right &&
            y >= node.rect.top && y < node.rect.bottom) {
            outPane = node.pane.get();
            outRect = node.rect;
            found = true;
        }
    });
    return found;
}

SplitNode* PaneManager::FindLeafInDirection(SplitNode* from,
                                             SplitDirection dir, bool forward) {
    SplitNode* current = from;

    while (current->parent) {
        SplitNode* par = current->parent;
        if (par->direction == dir) {
            bool isFirst = (par->first.get() == current);
            if (forward && isFirst) {
                SplitNode* target = par->second.get();
                while (!target->IsLeaf()) {
                    if (target->direction == dir)
                        target = target->first.get();
                    else
                        target = target->first.get();
                }
                return target;
            }
            if (!forward && !isFirst) {
                SplitNode* target = par->first.get();
                while (!target->IsLeaf()) {
                    if (target->direction == dir)
                        target = target->second.get();
                    else
                        target = target->first.get();
                }
                return target;
            }
        }
        current = par;
    }
    return nullptr;
}

SplitNode* PaneManager::FindSeparatorAtPoint(float x, float y) {
    std::vector<SeparatorLine> seps;
    CollectSeparators(seps);

    // Hit test margin for easier grabbing
    constexpr float MARGIN = 4.0f;

    std::function<SplitNode*(SplitNode*)> findNode = [&](SplitNode* node) -> SplitNode* {
        if (!node || node->IsLeaf()) return nullptr;

        // Check this node's separator
        if (node->direction == SplitDirection::Vertical) {
            float sepX = node->first->rect.right;
            if (x >= sepX - MARGIN && x <= sepX + SEP_WIDTH + MARGIN &&
                y >= node->rect.top && y <= node->rect.bottom) {
                return node;
            }
        } else {
            float sepY = node->first->rect.bottom;
            if (y >= sepY - MARGIN && y <= sepY + SEP_WIDTH + MARGIN &&
                x >= node->rect.left && x <= node->rect.right) {
                return node;
            }
        }

        // Recurse
        SplitNode* result = findNode(node->first.get());
        if (result) return result;
        return findNode(node->second.get());
    };

    return findNode(m_root.get());
}

void PaneManager::UpdateSplitRatio(SplitNode* node, float x, float y,
                                     float cellWidth, float cellHeight) {
    if (!node || node->IsLeaf()) return;

    float newRatio = 0.5f;

    if (node->direction == SplitDirection::Vertical) {
        float totalW = node->rect.right - node->rect.left;
        float relX = x - node->rect.left;
        newRatio = relX / totalW;

        // Calculate minimum ratio based on MIN_COLS
        float minFirstW = MIN_COLS * cellWidth;
        float minSecondW = MIN_COLS * cellWidth;
        float minRatio = (minFirstW + SEP_WIDTH) / totalW;
        float maxRatio = (totalW - minSecondW - SEP_WIDTH) / totalW;

        newRatio = (std::max)(minRatio, (std::min)(newRatio, maxRatio));
    } else {
        float totalH = node->rect.bottom - node->rect.top;
        float relY = y - node->rect.top;
        newRatio = relY / totalH;

        // Calculate minimum ratio based on MIN_ROWS
        float minFirstH = MIN_ROWS * cellHeight;
        float minSecondH = MIN_ROWS * cellHeight;
        float minRatio = (minFirstH + SEP_WIDTH) / totalH;
        float maxRatio = (totalH - minSecondH - SEP_WIDTH) / totalH;

        newRatio = (std::max)(minRatio, (std::min)(newRatio, maxRatio));
    }

    node->splitRatio = newRatio;

    // Relayout with provided cell dimensions
    LayoutNode(m_root.get(), m_fullRect, cellWidth, cellHeight);
}

float PaneManager::CalculateOverlap(D2D1_RECT_F r1, D2D1_RECT_F r2, SplitDirection dir) {
    // Calculate overlap along the perpendicular axis
    if (dir == SplitDirection::Vertical) {
        // For horizontal movement, calculate vertical overlap
        float top = (std::max)(r1.top, r2.top);
        float bottom = (std::min)(r1.bottom, r2.bottom);
        return (std::max)(0.0f, bottom - top);
    } else {
        // For vertical movement, calculate horizontal overlap
        float left = (std::max)(r1.left, r2.left);
        float right = (std::min)(r1.right, r2.right);
        return (std::max)(0.0f, right - left);
    }
}

SplitNode* PaneManager::FindPhysicalNeighbor(SplitNode* from, SplitDirection dir, bool forward) {
    if (!from || !from->IsLeaf()) return nullptr;

    D2D1_RECT_F fromRect = from->rect;
    SplitNode* bestMatch = nullptr;
    float bestOverlap = 0.0f;

    ForEachLeaf([&](SplitNode& node) {
        if (&node == from) return;

        D2D1_RECT_F nodeRect = node.rect;
        bool isNeighbor = false;

        // Check if this pane is in the correct direction
        if (dir == SplitDirection::Vertical) {
            if (forward) {
                // Right: node should be to the right of from
                isNeighbor = nodeRect.left >= fromRect.right - 1.0f;
            } else {
                // Left: node should be to the left of from
                isNeighbor = nodeRect.right <= fromRect.left + 1.0f;
            }
        } else {
            if (forward) {
                // Down: node should be below from
                isNeighbor = nodeRect.top >= fromRect.bottom - 1.0f;
            } else {
                // Up: node should be above from
                isNeighbor = nodeRect.bottom <= fromRect.top + 1.0f;
            }
        }

        if (isNeighbor) {
            float overlap = CalculateOverlap(fromRect, nodeRect, dir);
            if (overlap > bestOverlap) {
                bestOverlap = overlap;
                bestMatch = &node;
            }
        }
    });

    return bestMatch;
}

void PaneManager::SwapPaneContent(SplitDirection dir, bool forward) {
    if (!m_activeNode || !m_activeNode->IsLeaf() || m_zoomed) return;

    SplitNode* neighbor = FindPhysicalNeighbor(m_activeNode, dir, forward);
    if (!neighbor) return;

    // Swap pane contents (ConPty, Buffer, Parser)
    std::swap(m_activeNode->pane, neighbor->pane);
    std::swap(m_activeNode->paneId, neighbor->paneId);

    // Follow the content to neighbor
    m_activeNode = neighbor;
}

void PaneManager::MovePane(SplitDirection dir, bool forward) {
    if (!m_activeNode || !m_activeNode->IsLeaf() || m_zoomed) return;

    SplitNode* neighbor = FindPhysicalNeighbor(m_activeNode, dir, forward);
    if (!neighbor) return;

    SplitNode* current = m_activeNode;
    SplitNode* parent = current->parent;

    // Simple case: siblings with matching split direction
    if (parent && parent->direction == dir) {
        bool isFirst = (parent->first.get() == current);
        bool canSwap = (forward && isFirst) || (!forward && !isFirst);

        if (canSwap) {
            // Simple swap of siblings
            std::swap(parent->first, parent->second);
            return;
        }
    }

    // Complex case: need to restructure tree
    // For now, just swap content (same as SwapPaneContent)
    // Full tree restructuring is complex and needs careful parent pointer management
    SwapPaneContent(dir, forward);

    // Move focus to neighbor (content was swapped, so we follow the content)
    m_activeNode = neighbor;
}
