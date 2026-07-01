#include <doctest/doctest.h>
#include "../Runtime/Framework/EditorState.h"

TEST_CASE("SelectionState initialization") {
    SelectionState s;
    CHECK(s.selectedEntityId == 0);
    CHECK(s.gizmoOp == 0);   // ImGuizmo::TRANSLATE
    CHECK(s.gizmoMode == 0); // ImGuizmo::WORLD
    CHECK(s.renaming == false);
}

TEST_CASE("PendingActions initialization") {
    PendingActions p;
    CHECK(p.pendingDropPaths.empty());
    CHECK(p.requestSaveConfig == false);
}

TEST_CASE("HistoryState initialization") {
    HistoryState h;
    CHECK(h.requestUndo == false);
    CHECK(h.historyCursor == -1);
}
