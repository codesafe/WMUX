#include "pane/pane_tree.h"
#include "pane/pane_factory.h"
#include <algorithm>

bool PaneManager::Initialize(D2D1_RECT_F clientRect, HWND hwnd, UINT ptyMsg,
                              float cellWidth, float cellHeight,
                              const std::wstring& workingDir) {
    m_fullRect = clientRect;
    m_root = std::make_unique<SplitNode>();
    m_root->paneId = m_nextPaneId++;
    m_root->pane = CreatePaneSession();
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

bool PaneManager::InitializeWithSession(D2D1_RECT_F clientRect, HWND hwnd, UINT ptyMsg,
                                        float cellWidth, float cellHeight,
                                        std::unique_ptr<IPaneSession> session) {
    if (!session)
        return false;

    m_fullRect = clientRect;
    m_root = std::make_unique<SplitNode>();
    m_root->paneId = m_nextPaneId++;
    m_root->pane = std::move(session);
    m_root->rect = clientRect;

    constexpr float PADDING = 8.0f;
    float availW = (clientRect.right - clientRect.left) - PADDING;
    float availH = (clientRect.bottom - clientRect.top) - PADDING;
    int cols = (std::max)(1, static_cast<int>(availW / cellWidth));
    int rows = (std::max)(1, static_cast<int>(availH / cellHeight));

    if (!m_root->pane->Start(cols, rows, hwnd, ptyMsg, L"", m_root->paneId))
        return false;

    m_activeNode = m_root.get();
    return true;
}

bool PaneManager::SplitActive(SplitDirection dir, HWND hwnd, UINT ptyMsg,
                               float cellWidth, float cellHeight,
                               const std::wstring& workingDir) {
    auto pane = CreatePaneSession();
    if (!pane)
        return false;
    return SplitActiveWithSession(dir, std::move(pane), hwnd, ptyMsg,
                                  cellWidth, cellHeight, m_nextPaneId++, workingDir);
}

bool PaneManager::SplitActiveWithSession(SplitDirection dir, std::unique_ptr<IPaneSession> pane,
                               HWND hwnd, UINT ptyMsg,
                               float cellWidth, float cellHeight,
                               uint32_t paneId,
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
    newBranch->second->paneId = paneId;
    newBranch->second->pane = std::move(pane);
    newBranch->second->parent = newBranch.get();

    // Calculate correct size for new pane before starting
    constexpr float PADDING = 8.0f;
    int newCols = 10, newRows = 5;

    if (dir == SplitDirection::Vertical) {
        float half = (availW - SEP_WIDTH) / 2.0f;
        float availNewW = half - PADDING;
        float availNewH = availH - PADDING;
        newCols = (std::max)(1, static_cast<int>(availNewW / cellWidth));
        newRows = (std::max)(1, static_cast<int>(availNewH / cellHeight));
    } else {
        float half = (availH - SEP_WIDTH) / 2.0f;
        float availNewW = availW - PADDING;
        float availNewH = half - PADDING;
        newCols = (std::max)(1, static_cast<int>(availNewW / cellWidth));
        newRows = (std::max)(1, static_cast<int>(availNewH / cellHeight));
    }

    // Start new pane with correct size
    if (!newBranch->second->pane->Start(newCols, newRows, hwnd, ptyMsg, L"",
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

bool PaneManager::AttachToActive(std::unique_ptr<IPaneSession> pane, HWND /*hwnd*/, UINT /*ptyMsg*/,
                                 float cellWidth, float cellHeight, uint32_t paneId, int zone) {
    if (!m_activeNode || !m_activeNode->IsLeaf() || !pane)
        return false;

    if (zone == 4) {
        m_activeNode->pane->Stop();
        m_activeNode->pane = std::move(pane);
        m_activeNode->paneId = paneId;
        return true;
    }

    SplitDirection dir = (zone == 0 || zone == 2) ? SplitDirection::Horizontal : SplitDirection::Vertical;
    bool insertFirst = (zone == 0 || zone == 3);
    SplitNode* oldLeaf = m_activeNode;
    D2D1_RECT_F parentRect = oldLeaf->rect;

    auto newBranch = std::make_unique<SplitNode>();
    newBranch->direction = dir;
    newBranch->splitRatio = 0.5f;
    newBranch->rect = parentRect;
    newBranch->parent = oldLeaf->parent;

    auto movedLeaf = std::make_unique<SplitNode>();
    movedLeaf->pane = std::move(oldLeaf->pane);
    movedLeaf->paneId = oldLeaf->paneId;
    movedLeaf->parent = newBranch.get();

    auto attachedLeaf = std::make_unique<SplitNode>();
    attachedLeaf->pane = std::move(pane);
    attachedLeaf->paneId = paneId;
    attachedLeaf->parent = newBranch.get();

    if (insertFirst) {
        newBranch->first = std::move(attachedLeaf);
        newBranch->second = std::move(movedLeaf);
        m_activeNode = newBranch->first.get();
    } else {
        newBranch->first = std::move(movedLeaf);
        newBranch->second = std::move(attachedLeaf);
        m_activeNode = newBranch->second.get();
    }

    SplitNode* parent = oldLeaf->parent;
    if (!parent) {
        m_root = std::move(newBranch);
    } else if (parent->first.get() == oldLeaf) {
        parent->first = std::move(newBranch);
    } else {
        parent->second = std::move(newBranch);
    }

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

bool PaneManager::RemoveActiveWithoutStopping() {
    if (!m_activeNode) return false;

    if (m_zoomed) {
        m_zoomed = false;
    }

    // Release ownership without stopping - caller has already called PrepareForMove()
    // The pane's destructor handles cleanup (pipe disconnect but not host shutdown)
    m_activeNode->pane.reset();

    SplitNode* parent = m_activeNode->parent;
    if (!parent) {
        m_root.reset();
        m_activeNode = nullptr;
        return true;
    }

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

IPaneSession* PaneManager::FindPaneById(uint32_t id) {
    return FindPaneByIdRecursive(m_root.get(), id);
}

IPaneSession* PaneManager::FindPaneByIdRecursive(SplitNode* node, uint32_t id) {
    if (!node) return nullptr;
    if (node->IsLeaf()) {
        return (node->paneId == id) ? node->pane.get() : nullptr;
    }
    IPaneSession* p = FindPaneByIdRecursive(node->first.get(), id);
    if (p) return p;
    return FindPaneByIdRecursive(node->second.get(), id);
}

IPaneSession* PaneManager::GetActivePane() {
    return m_activeNode ? m_activeNode->pane.get() : nullptr;
}

const IPaneSession* PaneManager::GetActivePane() const {
    return m_activeNode ? m_activeNode->pane.get() : nullptr;
}

void PaneManager::SetActivePane(IPaneSession* pane) {
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

IPaneSession* PaneManager::FindPaneAtPoint(float x, float y) {
    IPaneSession* result = nullptr;
    ForEachLeaf([&](SplitNode& node) {
        if (x >= node.rect.left && x < node.rect.right &&
            y >= node.rect.top && y < node.rect.bottom) {
            result = node.pane.get();
        }
    });
    return result;
}

bool PaneManager::FindPaneAndRectAtPoint(float x, float y,
                                          IPaneSession*& outPane, D2D1_RECT_F& outRect) {
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

void PaneManager::InsertPaneAt(IPaneSession* source, IPaneSession* target, int zone) {
    if (!source || !target || source == target) return;

    // Find nodes
    SplitNode* sourceNode = nullptr;
    SplitNode* targetNode = nullptr;

    ForEachLeaf([&](SplitNode& node) {
        if (node.pane.get() == source) sourceNode = &node;
        if (node.pane.get() == target) targetNode = &node;
    });

    if (!sourceNode || !targetNode) return;

    // Zone 4: Center (swap)
    if (zone == 4) {
        std::swap(sourceNode->pane, targetNode->pane);
        std::swap(sourceNode->paneId, targetNode->paneId);
        m_activeNode = targetNode;
        return;
    }

    // Extract source pane from tree
    SplitNode* sourceParent = sourceNode->parent;
    std::unique_ptr<IPaneSession> extractedPane = std::move(sourceNode->pane);
    uint32_t extractedId = sourceNode->paneId;

    if (!sourceParent) {
        // Source is root - can't extract
        sourceNode->pane = std::move(extractedPane);
        return;
    }

    // Collapse source parent (promote sibling)
    bool sourceWasFirst = (sourceParent->first.get() == sourceNode);
    std::unique_ptr<SplitNode> sibling = std::move(sourceWasFirst ? sourceParent->second : sourceParent->first);

    SplitNode* grandParent = sourceParent->parent;
    if (!grandParent) {
        // Parent was root
        sibling->parent = nullptr;
        m_root = std::move(sibling);
    } else {
        // Replace parent with sibling
        bool parentWasFirst = (grandParent->first.get() == sourceParent);
        if (parentWasFirst) {
            grandParent->first = std::move(sibling);
            grandParent->first->parent = grandParent;
        } else {
            grandParent->second = std::move(sibling);
            grandParent->second->parent = grandParent;
        }
    }

    // Create new node for extracted pane
    auto newSourceNode = std::make_unique<SplitNode>();
    newSourceNode->pane = std::move(extractedPane);
    newSourceNode->paneId = extractedId;

    // Insert at target
    SplitDirection splitDir = (zone == 0 || zone == 2) ? SplitDirection::Horizontal : SplitDirection::Vertical;
    bool sourceFirst = (zone == 0 || zone == 3);  // top or left

    auto newBranch = std::make_unique<SplitNode>();
    newBranch->direction = splitDir;
    newBranch->splitRatio = 0.5f;

    // Move target pane to new branch
    auto targetCopy = std::make_unique<SplitNode>();
    targetCopy->pane = std::move(targetNode->pane);
    targetCopy->paneId = targetNode->paneId;

    if (sourceFirst) {
        newBranch->first = std::move(newSourceNode);
        newBranch->second = std::move(targetCopy);
    } else {
        newBranch->first = std::move(targetCopy);
        newBranch->second = std::move(newSourceNode);
    }

    newBranch->first->parent = newBranch.get();
    newBranch->second->parent = newBranch.get();

    // Replace target in tree
    SplitNode* targetParent = targetNode->parent;
    SplitNode* newBranchPtr = newBranch.get();

    if (!targetParent) {
        // Target is root
        newBranch->parent = nullptr;
        m_root = std::move(newBranch);
    } else {
        bool targetWasFirst = (targetParent->first.get() == targetNode);
        newBranch->parent = targetParent;

        if (targetWasFirst) {
            targetParent->first = std::move(newBranch);
        } else {
            targetParent->second = std::move(newBranch);
        }
    }

    // Set active to the moved source pane
    m_activeNode = sourceFirst ? newBranchPtr->first.get() : newBranchPtr->second.get();
}
