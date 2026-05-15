// Phase 4.2 / FR-1.3 / NFR-5.1 — central registry of file-scope Scene
// instances and the SceneId → Scene* dispatcher.
//
// Adding a new scene is two edits:
//   1. New `SceneId` enum value in scene_state.h
//   2. New file-scope instance + new switch case in scene_registry.cpp
//
// Instances are static (no heap, NFR-2.2) and outlive the program;
// returned pointers are valid forever.

#pragma once

#include "scene_state.h"

class Scene;
class FontDemoScene;

namespace scene_registry {

// Resolve a SceneId to its file-scope Scene instance. Returns nullptr
// for unknown ids — defensive default per FR-1.3 (malformed traffic
// must not crash).
Scene* scene_for(scene_state::SceneId id);

// Default scene the renderer points at before the first request().
// Today: the giant room-clock view (phase 3.5.3).
Scene* default_scene();

// Hard reference for IR action wiring (LEFT/RIGHT carve-out for the
// font diagnostic). Returned pointer never changes.
FontDemoScene* font_demo();

}  // namespace scene_registry
