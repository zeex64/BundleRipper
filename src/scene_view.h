// The window's Scene page: a 3D view of the converted map with its object list and properties.
#pragma once
#include "gui_common.h"
#include "job.h"

#include <d3d11.h>

#include <memory>
#include <string>

namespace xl {

struct SceneViewStatus {
    bool busy = false;      // a rip or a load is running
    bool loading = false;   // ...and it is this view's load
    std::string step;       // what the load is doing
    std::string error;      // why the last load failed
    bool can_load = false;  // a map is chosen
    bool stale = false;     // the map or options changed since the shown scene was loaded
};

enum class SceneAction { None, Load, OpenFile, OpenFolder };

class SceneView {
public:
    SceneView(ID3D11Device* device, ID3D11DeviceContext* context, const ui::Fonts& fonts);
    ~SceneView();
    // Takes a loaded map and uploads it to the GPU (replacing the one shown).
    void set(Preview&& preview);
    void clear();
    bool loaded() const;
    // Draws the object list, view and inspector into the available region; returns what the
    // user asked for. LOD choices in `job` switch live; the plant rows edit job.opt.plant_lod.
    // `edits` are shown live and changed by the inspector.
    SceneAction draw(const SceneViewStatus& status, Job& job, Edits& edits);
    // True once after the inspector changed the edits (the window then saves them).
    bool take_edits_changed();
    // The window swapped in other edits (another map was opened).
    void edits_replaced();
    // The window's undo or redo put back earlier edits: everything redraws, the selection stays
    // where it still exists.
    void edits_restored();
    // A short message for the window to show (copied, pasted...), once.
    std::string take_message();
    // Makes the loaded map's lights (not suns) editable lights in `edits`, with the map's values,
    // unless they already are or were deleted. True when it added any.
    bool import_map_lights(Edits& edits);
    // Leaves out lights made from the map that nobody changed (they are made again on load), so
    // the file beside the map only holds real changes.
    void drop_unchanged_imports(Edits& edits) const;
    size_t unchanged_imports(const Edits& edits) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xl
