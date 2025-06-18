// Copyright © 2025 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _VR_SYSTEM_H
#define _VR_SYSTEM_H

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "graphics/opengl/TextureGL.h"

#include <optional>
#include <vector>

#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#include <wingdi.h>
#elif defined(__linux__)
#define XR_USE_PLATFORM_XLIB

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

	bool Init();
	void Update();

	bool ShouldRender() const;

	void BeginFrame();
	void EndFrame();

	void SetRenderer(Graphics::Renderer *renderer) { m_renderer = renderer; }

protected:
	bool CreateSession();
	bool CreateSwapchains();

	Graphics::Renderer *m_renderer;
	Graphics::RenderTarget *m_renderTargets[2];

	bool m_readyToRender = false;

	XrInstance m_xrInstance;
	XrSystemId m_xrSystemID;
	XrSession m_xrSession;

	XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;

	PFN_xrGetOpenGLGraphicsRequirementsKHR m_xrGetOpenGLGraphicsRequirementsKHR;

	XrSpace m_refSpace;
	XrFrameState m_frameState = {XR_TYPE_FRAME_STATE};

	XrViewState m_viewState = {XR_TYPE_VIEW_STATE};
	XrView m_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XrCompositionLayerProjectionView m_projectionLayerViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	XrViewConfigurationView m_configViews[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

	XrSwapchain m_colorSwapchains[2];
	XrSwapchain m_depthSwapchains[2];
	std::vector<XrSwapchainImageOpenGLKHR> m_colorImages[2];
	std::vector<XrSwapchainImageOpenGLKHR> m_depthImages[2];

	Graphics::OGL::TextureGL m_renderTexturesColor[2];
	Graphics::OGL::TextureGL m_renderTexturesDepth[2];
};

class VR {
public:
	static void Init(Graphics::Renderer *renderer) {
		m_system.SetRenderer(renderer);
		m_system.Init();
	}

	static void Update() { m_system.Update(); }

	static void BeginFrame() { m_system.BeginFrame(); }

	static void EndFrame() { m_system.EndFrame(); }

	static void SetupRenderingForEye(int eye) { m_system.SetupRenderingForEye(eye); }

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
};

#pragma GCC diagnostic pop

#endif /* _VR_SYSTEM_H */
