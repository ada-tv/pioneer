// Copyright © 2025 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _VR_SYSTEM_H
#define _VR_SYSTEM_H

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "graphics/opengl/TextureGL.h"

#include <vector>

#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#include <wingdi.h>
#elif defined(__linux__)
#define XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_EGL

#include <EGL/egl.h>

// Xlib is such a pain
#define Time Time_HorribleXLibHack
#define None None_HorribleXLibHack
#define Success Success_HorribleXLibHack
#define Failed Failed_HorribleXLibHack
#define Bool Bool_HorribleXLibHack
#include <GL/glx.h>
#undef Time
#undef None
#undef Success
#undef Failed
#undef Bool

#else
#error "Unsupported platform"
#endif

#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "matrix4x4.h"

namespace Graphics {
	class Renderer;
	class RenderTarget;
}

class VRSystem {
public:
	VRSystem();
	VRSystem(Graphics::Renderer *renderer);
	~VRSystem();

	matrix4x4f GetProjection(int eye);
	matrix4x4f GetView(int eye);
	void SetupRenderingForEye(int eye);
	void SetupRenderingForHUD();
	void DrawDesktopMirror();

	bool Init();
	void Update();

	bool ShouldRender() const;

	void BeginFrame();
	void EndFrame();

	void SetRenderer(Graphics::Renderer *renderer) { m_renderer = renderer; }

protected:
	static constexpr float hudLayerSize[2] = {2.0f, 2.0f / (16.0f / 9.0f)};

	static constexpr int RENDER_TARGET_COUNT = 3;
	static constexpr int RENDER_TARGET_EYE_COUNT = 2;
	static constexpr int TARGET_LEFT = 0;
	static constexpr int TARGET_RIGHT = 1;
	static constexpr int TARGET_HUD = 2;

	bool CreateSession();
	bool CreateSwapchains();
	XrResult CheckResult(XrResult result);

	Graphics::Renderer *m_renderer;
	Graphics::RenderTarget *m_renderTargets[RENDER_TARGET_COUNT];

	bool m_readyToRender = false;
	bool m_cylinderSupported = false;

	XrInstance m_xrInstance;
	XrSystemId m_xrSystemID;
	XrSession m_xrSession;

	XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;

	PFN_xrGetOpenGLGraphicsRequirementsKHR m_xrGetOpenGLGraphicsRequirementsKHR;

	XrSpace m_refSpace;
	XrFrameState m_frameState = {XR_TYPE_FRAME_STATE};

	XrViewState m_viewState = {XR_TYPE_VIEW_STATE};
	XrView m_views[RENDER_TARGET_EYE_COUNT] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XrViewConfigurationView m_configViews[RENDER_TARGET_EYE_COUNT] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XrCompositionLayerProjectionView m_projectionLayerViews[RENDER_TARGET_EYE_COUNT] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	XrCompositionLayerQuad m_hudLayerViewQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerCylinderKHR m_hudLayerViewCylinder = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};

	XrSwapchain m_colorSwapchains[RENDER_TARGET_COUNT];
	std::vector<XrSwapchainImageOpenGLKHR> m_colorImages[RENDER_TARGET_COUNT];

	Graphics::OGL::TextureGL m_renderTexturesColor[RENDER_TARGET_COUNT];
};

class VR {
public:
	static constexpr int hudResolution[2] = {1280, 720};

	static void Init(Graphics::Renderer *renderer) {
		m_system.SetRenderer(renderer);
		m_system.Init();
	}

	static void Update() { m_system.Update(); }

	static void BeginFrame() { m_system.BeginFrame(); }

	static void DrawDesktopMirror() { m_system.DrawDesktopMirror(); }

	static void EndFrame() { m_system.EndFrame(); }

	static void SetupRenderingForEye(int eye) {
		m_isRenderingHud = false;
		m_system.SetupRenderingForEye(eye);
	}

	static void SetupRenderingForHUD() {
		m_isRenderingHud = true;
		m_system.SetupRenderingForHUD();
	}

	static bool IsRenderingHUD() { return m_isRenderingHud; }

	static bool ShouldRender() { return m_isActive && m_system.ShouldRender(); }

	static bool IsActive() { return m_isActive; }

	static int ActiveEye() { return m_activeEye; }

	static void SetActiveEye(int eye) { m_activeEye = eye; }

	static matrix4x4f GetEyeProjection(int eye) { return m_system.GetProjection(eye); }

	static matrix4x4f GetEyeView(int eye) { return m_system.GetView(eye); }

private:
	friend class VRSystem;

	static VRSystem m_system;

	static bool m_isActive;
	static int m_activeEye;
	static bool m_isRenderingHud;
};

#pragma GCC diagnostic pop

#endif /* _VR_SYSTEM_H */
