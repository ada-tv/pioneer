// Copyright © 2025 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include <cstring>

#include "graphics/Graphics.h"
#include "graphics/opengl/RendererGL.h"
#include "xr_linear.h"

#include "core/Log.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "Pi.h"
#include <openxr/openxr.h>
#include "VRSystem.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

VRSystem VR::m_system;
bool VR::m_isActive = false;
int VR::m_activeEye = 0;
bool VR::m_isRenderingHud = false;

using Graphics::OGL::TextureGL;
using Graphics::TextureDescriptor;

VRSystem::VRSystem(Graphics::Renderer *renderer) :
	m_renderer(renderer),
	m_renderTexturesColor {
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()),
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()),
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()) } {
}

VRSystem::VRSystem() :
	m_renderer(nullptr),
	m_renderTexturesColor {
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()),
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()),
		TextureGL(0, GL_TEXTURE_2D, TextureDescriptor()) } {
}

VRSystem::~VRSystem() {
}

matrix4x4f VRSystem::GetProjection(int eye) {
	constexpr float nearZ = 0.05f;

	// D3D matrix because we're using Y-up, 0,1 NDC rather than Y-up -1,1 NDC,
	// far can be any negative number and CreateProjectionFov will make an infinite projection
	XrMatrix4x4f proj;
	XrMatrix4x4f_CreateProjectionFov(&proj, GRAPHICS_D3D, m_views[eye].fov, nearZ, -1000.0f);

	// weird magic that has to be here or the depth breaks
	proj.m[10] = 0.0f;
	proj.m[14] = nearZ;

	return matrix4x4f(proj.m);
}

matrix4x4f VRSystem::GetView(int eye) {
	XrVector3f scale = {1.0f, 1.0f, 1.0f};
	XrMatrix4x4f view, toView;
	XrMatrix4x4f_CreateTranslationRotationScale(&toView, &m_views[eye].pose.position, &m_views[eye].pose.orientation, &scale);
	XrMatrix4x4f_InvertRigidBody(&view, &toView);

	return matrix4x4f(view.m);
}

bool VRSystem::Init() {
	XrResult result;

	if (!CreateSession()) { return false; }

	XrReferenceSpaceCreateInfo spaceInfo = {
		XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		nullptr,
		XR_REFERENCE_SPACE_TYPE_LOCAL,
		{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}
	};
	result = xrCreateReferenceSpace(m_xrSession, &spaceInfo, &m_refSpace);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrCreateReferenceSpace: {}", buffer);
		return false;
	}

	uint32_t viewConfigCount = 2;
	result = xrEnumerateViewConfigurationViews(m_xrInstance, m_xrSystemID, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewConfigCount, &viewConfigCount, m_configViews);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrEnumerateViewConfigurationViews: {}", buffer);
		return false;
	}

	if (!CreateSwapchains()) { return false; }

	// eye views
	for (int i = 0; i < RENDER_TARGET_EYE_COUNT; i++) {
		m_projectionLayerViews[i].subImage.swapchain = m_colorSwapchains[i];
		m_projectionLayerViews[i].subImage.imageArrayIndex = 0;
		m_projectionLayerViews[i].subImage.imageRect.offset.x = 0;
		m_projectionLayerViews[i].subImage.imageRect.offset.y = 0;
		m_projectionLayerViews[i].subImage.imageRect.extent.width = m_configViews[i].recommendedImageRectWidth;
		m_projectionLayerViews[i].subImage.imageRect.extent.height = m_configViews[i].recommendedImageRectHeight;
	}

	// hud quad layer
	m_hudLayerView.size.width = hudLayerSize[0];
	m_hudLayerView.size.height = hudLayerSize[1];
	m_hudLayerView.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	m_hudLayerView.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	m_hudLayerView.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_hudLayerView.pose.position = { 0.0f, 0.0f, -1.0f };
	m_hudLayerView.subImage.swapchain = m_colorSwapchains[TARGET_HUD];
	m_hudLayerView.subImage.imageArrayIndex = 0;
	m_hudLayerView.subImage.imageRect.offset.x = 0;
	m_hudLayerView.subImage.imageRect.offset.y = 0;
	m_hudLayerView.subImage.imageRect.extent.width = VR::hudResolution[0];
	m_hudLayerView.subImage.imageRect.extent.height = VR::hudResolution[1];
	m_hudLayerView.space = m_refSpace;

	// don't vsync to monitor refresh rate,
	// openxr handles sync for us
	m_renderer->SetVSyncEnabled(false);

	VR::m_isActive = true;
	return true;
}

bool VRSystem::CreateSession() {
	static const char *const extensions[] = {
		XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
	};

	XrResult result;

	XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(instanceInfo.applicationInfo.applicationName, "Pioneer", XR_MAX_APPLICATION_NAME_SIZE - 1);
	instanceInfo.applicationInfo.applicationVersion = XR_MAKE_VERSION(2025, 05, 01);
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.enabledExtensionNames = extensions;
	instanceInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);

	result = xrCreateInstance(&instanceInfo, &m_xrInstance);
	if (result) {
		Log::Fatal("xrCreateInstance");
		return false;
	}

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	result = xrGetSystem(m_xrInstance, &systemInfo, &m_xrSystemID);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrGetSystem: {}", buffer);
		return false;
	}

	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.systemId = m_xrSystemID;

	xrGetInstanceProcAddr(m_xrInstance, "xrGetOpenGLGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&m_xrGetOpenGLGraphicsRequirementsKHR));

	XrGraphicsRequirementsOpenGLKHR requirementsDummy = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
	m_xrGetOpenGLGraphicsRequirementsKHR(m_xrInstance, m_xrSystemID, &requirementsDummy);

	#if defined(_WIN32)
	XrGraphicsBindingOpenGLWin32KHR graphicsBindingGL = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
	graphicsBindingGL.hDC = wglGetCurrentDC();
	graphicsBindingGL.hGLRC = wglGetCurrentContext();

	sessionInfo.next = &graphicsBindingGL;
	#elif defined(__linux__)
	XrGraphicsBindingOpenGLXlibKHR graphicsBindingGL = {XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
	graphicsBindingGL.xDisplay = XOpenDisplay(nullptr);
	graphicsBindingGL.glxDrawable = glXGetCurrentDrawable();
	graphicsBindingGL.glxContext = glXGetCurrentContext();

	sessionInfo.next = &graphicsBindingGL;
	#endif

	result = xrCreateSession(m_xrInstance, &sessionInfo, &m_xrSession);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrCreateSession: {}", buffer);
		return false;
	}

	return true;
}

bool VRSystem::CreateSwapchains() {
	// eye swapchains
	for (int i = 0; i < RENDER_TARGET_EYE_COUNT; i++) {
		XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		swapchainInfo.format = GL_SRGB8_ALPHA8;
		swapchainInfo.sampleCount = 1;
		swapchainInfo.width = m_configViews[i].recommendedImageRectWidth;
		swapchainInfo.height = m_configViews[i].recommendedImageRectHeight;
		swapchainInfo.faceCount = 1;
		swapchainInfo.arraySize = 1;
		swapchainInfo.mipCount = 1;

		XrResult result = xrCreateSwapchain(m_xrSession, &swapchainInfo, &m_colorSwapchains[i]);
		if (result) {
			char buffer[XR_MAX_RESULT_STRING_SIZE];
			xrResultToString(m_xrInstance, result, buffer);
			Log::Fatal("xrCreateSwapchain: {}", buffer);
			return false;
		}

		uint32_t colorImgCount;
		xrEnumerateSwapchainImages(m_colorSwapchains[i], 0, &colorImgCount, nullptr);

		m_colorImages[i].resize(colorImgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});

		xrEnumerateSwapchainImages(m_colorSwapchains[i], colorImgCount, &colorImgCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_colorImages[i].data()));

		Graphics::RenderTargetDesc rtDesc(
			swapchainInfo.width,
			swapchainInfo.height,
			Graphics::TextureFormat::TEXTURE_NONE,
			Graphics::TextureFormat::TEXTURE_DEPTH,
			false,
			0
		);

		m_renderTargets[i] = m_renderer->CreateRenderTarget(rtDesc);
	}

	// hud swapchain
	XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swapchainInfo.format = GL_SRGB8_ALPHA8;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = VR::hudResolution[0];
	swapchainInfo.height = VR::hudResolution[1];
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	XrResult result = xrCreateSwapchain(m_xrSession, &swapchainInfo, &m_colorSwapchains[TARGET_HUD]);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrCreateSwapchain: {}", buffer);
		return false;
	}

	uint32_t colorImgCount;
	xrEnumerateSwapchainImages(m_colorSwapchains[TARGET_HUD], 0, &colorImgCount, nullptr);

	m_colorImages[TARGET_HUD].resize(colorImgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});

	xrEnumerateSwapchainImages(m_colorSwapchains[TARGET_HUD], colorImgCount, &colorImgCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_colorImages[TARGET_HUD].data()));

	Graphics::RenderTargetDesc rtDesc(
		swapchainInfo.width,
		swapchainInfo.height,
		Graphics::TextureFormat::TEXTURE_NONE,
		Graphics::TextureFormat::TEXTURE_DEPTH,
		false,
		0
	);

	m_renderTargets[TARGET_HUD] = m_renderer->CreateRenderTarget(rtDesc);

	return true;
}

void VRSystem::BeginFrame() {
	if (!m_readyToRender) { return; }

	XrFrameWaitInfo waitFrameInfo = {XR_TYPE_FRAME_WAIT_INFO};
	xrWaitFrame(m_xrSession, &waitFrameInfo, &m_frameState);

	XrFrameBeginInfo beginFrameInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	xrBeginFrame(m_xrSession, &beginFrameInfo);

	XrViewLocateInfo viewLocateInfo = {
		XR_TYPE_VIEW_LOCATE_INFO,
		nullptr,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		m_frameState.predictedDisplayTime,
		m_refSpace
	};

	uint32_t dummy;
	XrResult result = xrLocateViews(m_xrSession, &viewLocateInfo, &m_viewState, 2, &dummy, m_views);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Fatal("xrLocateViews: {}", buffer);
	}

	for (int i = 0; i < RENDER_TARGET_COUNT; i++) {
		// only update the poses for the eye views, not the hud
		if (i < RENDER_TARGET_EYE_COUNT) {
			m_projectionLayerViews[i].pose = m_views[i].pose;
			m_projectionLayerViews[i].fov = m_views[i].fov;
		}

		uint32_t colorImageIndex;
		XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};

		xrAcquireSwapchainImage(m_colorSwapchains[i], &acquireInfo, &colorImageIndex);
		xrWaitSwapchainImage(m_colorSwapchains[i], &waitInfo);

		m_renderTexturesColor[i].SetTextureID(m_colorImages[i][colorImageIndex].image);
		m_renderTargets[i]->SetColorTexture(&m_renderTexturesColor[i]);

		m_renderer->SetRenderTarget(m_renderTargets[i]);
		m_renderer->ClearScreen(Color(0, 0, 0, 0), true);
	}
}

void VRSystem::DrawDesktopMirror() {
	// FIXME: can't scale unresolved MSAA targets
	// and even the hud doesn't get copied
#if 0
	// copy to the desktop window
	auto desktop_rt = Pi::GetApp()->GetRenderTarget();

	auto desktop_rt_desc = desktop_rt->GetDesc();

	auto left_desc = m_renderTargets[TARGET_LEFT]->GetDesc();
	auto right_desc = m_renderTargets[TARGET_LEFT]->GetDesc();

	m_renderer->CopyRenderTarget(
		m_renderTargets[TARGET_LEFT],
		desktop_rt,
		{0, 0, left_desc.width, left_desc.height},
		{0, 0, left_desc.width, left_desc.height}
	);

	m_renderer->CopyRenderTarget(
		m_renderTargets[TARGET_RIGHT],
		desktop_rt,
		{0, 0, right_desc.width, right_desc.height},
		{0, 0, right_desc.width, right_desc.height}
	);

	m_renderer->CopyRenderTarget(
		m_renderTargets[TARGET_HUD],
		desktop_rt,
		{0, 0, VR::hudResolution[0], VR::hudResolution[1]},
		{0, 0, VR::hudResolution[0], VR::hudResolution[1]}
	);

	m_renderer->FlushCommandBuffers();
#endif
}

void VRSystem::EndFrame() {
	if (!m_readyToRender) { return; }

	for (int i = 0; i < RENDER_TARGET_COUNT; i++) {
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(m_colorSwapchains[i], &releaseInfo);
	}

	XrCompositionLayerProjection projectionLayer = {
		XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		nullptr,
		0,
		m_refSpace,
		2,
		m_projectionLayerViews
	};

	XrCompositionLayerBaseHeader *layers[] = {
		reinterpret_cast<XrCompositionLayerBaseHeader *>(&projectionLayer),
		reinterpret_cast<XrCompositionLayerBaseHeader *>(&m_hudLayerView),
	};

	XrFrameEndInfo frameEndInfo = {
		XR_TYPE_FRAME_END_INFO,
		nullptr,
		m_frameState.predictedDisplayTime,
		XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		ShouldRender() ? 2u : 0u,
		layers
	};

	XrResult result = xrEndFrame(m_xrSession, &frameEndInfo);
	if (result) {
		char buffer[XR_MAX_RESULT_STRING_SIZE];
		xrResultToString(m_xrInstance, result, buffer);
		Log::Error("xrEndFrame: {}", buffer);
	}

	Log::Debug("VrSystem::EndFrame");
}

bool VRSystem::ShouldRender() const {
	//if (!m_frameState.shouldRender) { return false; }
	if (!m_readyToRender) { return false; }
	return true;
}

void VRSystem::Update() {
	XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER};
	XrResult pollResult = xrPollEvent(m_xrInstance, &eventData);

	while (pollResult == XR_SUCCESS) {
		switch (eventData.type) {
			case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
				const auto &event = *reinterpret_cast<XrEventDataEventsLost *>(&eventData);
				Log::Warning("Lost %d OpenXR events!", event.lostEventCount);
				break;
			}

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				Log::Fatal("OpenXR instance has been lost!");
				Pi::RequestQuit();
				break;
			}

			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto &event = *reinterpret_cast<XrEventDataSessionStateChanged *>(&eventData);

				m_sessionState = event.state;

				if (m_sessionState == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

					Log::Debug("OpenXR session beginning");

					XrResult result = xrBeginSession(m_xrSession, &beginInfo);
					if (result) {
						char buffer[XR_MAX_RESULT_STRING_SIZE];
						xrResultToString(m_xrInstance, result, buffer);
						Log::Error("xrBeginSession: {}", buffer);
					}
				} else if (m_sessionState == XR_SESSION_STATE_STOPPING) {
					Log::Debug("OpenXR session stopping");
					xrEndSession(m_xrSession);
					Pi::RequestQuit();
				} else if (m_sessionState == XR_SESSION_STATE_EXITING) {
					Log::Debug("OpenXR session exiting");
					Pi::RequestQuit();
				} else if (m_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
					Log::Fatal("OpenXR instance has been lost!");
					Pi::RequestQuit();
				}

				m_readyToRender = (
					m_sessionState == XR_SESSION_STATE_READY ||
					m_sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
					m_sessionState == XR_SESSION_STATE_VISIBLE ||
					m_sessionState == XR_SESSION_STATE_FOCUSED
				);

				break;
			}

			default: { break; }
		}

		eventData.type = XR_TYPE_EVENT_DATA_BUFFER;
		pollResult = xrPollEvent(m_xrInstance, &eventData);
	}
}

void VRSystem::SetupRenderingForEye(int eye) {
	auto &renderer = *static_cast<Graphics::RendererOGL *>(m_renderer);
	renderer.m_viewportOverride = Graphics::ViewportExtents(
		m_projectionLayerViews[eye].subImage.imageRect.offset.x,
		m_projectionLayerViews[eye].subImage.imageRect.offset.y,
		m_projectionLayerViews[eye].subImage.imageRect.extent.width,
		m_projectionLayerViews[eye].subImage.imageRect.extent.height
	);
	renderer.m_renderTargetOverride = m_renderTargets[eye];
	renderer.SetRenderTarget(renderer.m_renderTargetOverride);
	renderer.SetViewport(*renderer.m_viewportOverride);
}

void VRSystem::SetupRenderingForHUD() {
	auto &renderer = *static_cast<Graphics::RendererOGL *>(m_renderer);
	renderer.m_viewportOverride = Graphics::ViewportExtents(
		0, 0, VR::hudResolution[0], VR::hudResolution[1]
	);
	renderer.m_renderTargetOverride = m_renderTargets[TARGET_HUD];
	renderer.SetRenderTarget(renderer.m_renderTargetOverride);
	renderer.SetViewport(*renderer.m_viewportOverride);
}
