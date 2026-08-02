#pragma once
// VkAgentBridge.h — the engine-facing half of the command port.
//
// CommandServer owns sockets and queues and knows nothing about the engine;
// this owns the handlers, which need VkAppState, the renderer and the ECS.
// Same split as ScriptBindings/VkScriptBindings, for the same reason.
//
// Handlers are DELIBERATELY few. Phase 2 already bound the whole authoring API
// to Lua, so `script.eval` covers anything not listed here rather than the
// port growing a second, parallel implementation of it that can drift. What
// gets a typed method is what an MCP client wants structured parameters for,
// or what cannot be expressed as a single synchronous call:
//
//   engine.info        version, capabilities, frame counter
//   script.eval        arbitrary Lua, JSON result -- the general escape hatch
//   assets.generators  list, with descriptions and poly budgets
//   assets.schema      one generator's parameter schema
//   assets.define      generate + register a recipe
//   scene.spawn        place an asset
//   scene.query        what is in the scene
//   scene.clear        remove spawned entities
//   render.setParams   camera/light/post
//   render.getParams   read them back
//   render.capture     DEFERRED: the PNG exists a frame later
//   render.turntable   DEFERRED: N framed shots of one asset, many frames
//
// The last two are why the protocol carries request ids at all.

#include "Bridge/CommandServer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct VkAppState;

class VkAgentBridge {
public:
  // Registers handlers and starts listening on 127.0.0.1:port. Returns false
  // if the port could not be bound (message in server().lastError()).
  bool start(VkAppState &state, uint16_t port);
  void stop();
  bool running() const { return mServer.running(); }
  bridge::CommandServer &server() { return mServer; }

  // Call ONCE PER FRAME from the main loop, at the point documented in
  // main.cpp. Drains queued requests (running their handlers) and advances any
  // in-flight capture or turntable. Must run BEFORE drawFrame() so a capture
  // requested this frame is serviced by it.
  void update();

  // Call immediately AFTER drawFrame(). A capture is written during the draw,
  // so this is the earliest point its file is known to exist -- and therefore
  // the earliest a deferred reply can honestly be sent.
  void postFrame();

  // True on frames the bridge is capturing for an agent. main.cpp skips the
  // editor overlay while it holds, because the capture goes straight into a
  // model's context: docked ImGui panels cover roughly a third of the frame
  // and are pure noise when the question is "does this asset look right".
  bool wantsCleanFrame() const;

  // Longest side, in pixels, for bridge-issued captures.
  void setCaptureMaxDimension(uint32_t px) { mCaptureMaxDim = px; }

private:
  // In-flight screenshot: one request, one file, completed after the draw.
  struct PendingCapture {
    uint64_t token = 0;
    std::string path;
    bool requested = false; // requestCapture() already issued to the renderer
    uint32_t maxDim = 0;
    bool includeUi = false;
  };

  // In-flight turntable: the asset is re-framed and re-shot once per step.
  // A little state machine rather than a loop, because each shot needs its
  // own drawFrame -- there is no way to render N images synchronously.
  struct PendingTurntable {
    uint64_t token = 0;
    uint32_t entity = 0;
    std::string basePath;
    int steps = 8;
    int current = -1; // -1 = not yet started
    float radius = 6.0f;
    float centerY = 0.0f;
    float baseY = 0.0f;
    bool shotRequested = false;
    std::vector<std::string> paths;
    // Camera/light state to restore when the sequence finishes, so a
    // turntable does not silently leave the editor looking somewhere else.
    glm::vec3 savedCamPos{0.0f};
    float savedYaw = 0.0f, savedPitch = 0.0f;
    bool savedAutoExposure = false;
    float savedFixedTime = -1.0f;
    bool savedCameraGrade = true;
  };

  void registerHandlers_();

  VkAppState *mState = nullptr;
  bridge::CommandServer mServer;
  uint64_t mFrame = 0;

  PendingCapture mCapture;
  bool mCaptureActive = false;
  PendingTurntable mTurntable;
  bool mTurntableActive = false;
  // 0 = native resolution. Default keeps a capture small enough to inline
  // into a model's context; a client can raise it per request.
  uint32_t mCaptureMaxDim = 768;
  bool mHideUiThisFrame = false;
};
