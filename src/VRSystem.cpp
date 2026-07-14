// Copyright © 2025 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include <cstring>
#include <array>

#include "MathUtil.h"
#include "Quaternion.h"
#include "graphics/Graphics.h"
#include "graphics/opengl/RendererGL.h"

#include "core/Log.h"
#include "graphics/RenderTarget.h"
#include "graphics/Renderer.h"
#include "Pi.h"
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

XrResult VRSystem::CheckResult(XrResult result) {
	if (XR_SUCCEEDED(result)) { return result; }

	if (m_xrInstance) {
		Log::Error("OpenXR: {}", static_cast<uint32_t>(result));
	} else {
		char name[XR_MAX_RESULT_STRING_SIZE] = {};
		xrResultToString(m_xrInstance, result, name);

		Log::Error("OpenXR: {} ({})", name, static_cast<uint32_t>(result));
	}

	return result;
}

matrix4x4f VRSystem::GetProjection(int eye) {
	constexpr float nearZ = 0.05f;

	auto fov = m_views[eye].fov;
	fov.angleLeft = std::tan(fov.angleLeft);
	fov.angleRight = std::tan(fov.angleRight);
	fov.angleDown = std::tan(fov.angleDown);
	fov.angleUp = std::tan(fov.angleUp);

	auto fovX = fov.angleRight - fov.angleLeft;
	auto fovY = fov.angleUp - fov.angleDown;

	auto halfX = 2.0f / fovX;
	auto halfY = 2.0f / fovY;

	auto deltaX = (fov.angleRight + fov.angleLeft) / fovX;
	auto deltaY = (fov.angleUp + fov.angleDown) / fovY;

	float mat[16] = {
		halfX,  0.0f,   0.0f,   0.0f,
		0.0f,   halfY,  0.0f,   0.0f,
		deltaX, deltaY, 0.0f,  -1.0f,
		0.0f,   0.0f,   nearZ,  0.0f,
	};

	return matrix4x4f(mat);
}

matrix4x4f VRSystem::GetView(int eye) {
	auto pos = m_views[eye].pose.position;
	auto orient = m_views[eye].pose.orientation;
	auto orientMat = Quaternionf(orient.w, orient.x, orient.y, orient.z).ToMatrix3x3<float>();

	auto mat = matrix4x4f::Identity;
	mat.LoadFrom3x3Matrix(orientMat.Data());
	mat.SetTranslate(vector3f(pos.x, pos.y, pos.z));

	return mat.Inverse();
}

bool VRSystem::Init() {
	if (!CreateSession()) { return false; }

	XrReferenceSpaceCreateInfo spaceInfo = {
		XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		nullptr,
		XR_REFERENCE_SPACE_TYPE_LOCAL,
		{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}
	};
	CheckResult(xrCreateReferenceSpace(m_xrSession, &spaceInfo, &m_refSpace));

	uint32_t viewConfigCount = 2;
	CheckResult(xrEnumerateViewConfigurationViews(m_xrInstance, m_xrSystemID, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewConfigCount, &viewConfigCount, m_configViews));

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

	// hud layer
	if (m_cylinderSupported) {
		m_hudLayerViewCylinder.centralAngle = DEG2RAD(90.0f);
		m_hudLayerViewCylinder.radius = 1.0f;
		m_hudLayerViewCylinder.aspectRatio = static_cast<float>(VR::hudResolution[0]) / static_cast<float>(VR::hudResolution[1]);
		m_hudLayerViewCylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		m_hudLayerViewCylinder.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		m_hudLayerViewCylinder.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
		m_hudLayerViewCylinder.pose.position = { 0.0f, -0.1f, 0.0f };
		m_hudLayerViewCylinder.subImage.swapchain = m_colorSwapchains[TARGET_HUD];
		m_hudLayerViewCylinder.subImage.imageArrayIndex = 0;
		m_hudLayerViewCylinder.subImage.imageRect.offset.x = 0;
		m_hudLayerViewCylinder.subImage.imageRect.offset.y = 0;
		m_hudLayerViewCylinder.subImage.imageRect.extent.width = VR::hudResolution[0];
		m_hudLayerViewCylinder.subImage.imageRect.extent.height = VR::hudResolution[1];
		m_hudLayerViewCylinder.space = m_refSpace;
	} else {
		m_hudLayerViewQuad.size.width = hudLayerSize[0];
		m_hudLayerViewQuad.size.height = hudLayerSize[1];
		m_hudLayerViewQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		m_hudLayerViewQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		m_hudLayerViewQuad.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
		m_hudLayerViewQuad.pose.position = { 0.0f, -0.1f, -1.0f };
		m_hudLayerViewQuad.subImage.swapchain = m_colorSwapchains[TARGET_HUD];
		m_hudLayerViewQuad.subImage.imageArrayIndex = 0;
		m_hudLayerViewQuad.subImage.imageRect.offset.x = 0;
		m_hudLayerViewQuad.subImage.imageRect.offset.y = 0;
		m_hudLayerViewQuad.subImage.imageRect.extent.width = VR::hudResolution[0];
		m_hudLayerViewQuad.subImage.imageRect.extent.height = VR::hudResolution[1];
		m_hudLayerViewQuad.space = m_refSpace;
	}

	// don't vsync to monitor refresh rate,
	// openxr handles sync for us
	m_renderer->SetVSyncEnabled(false);

	VR::m_isActive = true;
	return true;
}

bool VRSystem::CreateSession() {
	std::vector<const char *> extensions = {
		XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
		XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
	};

	uint32_t propertyCount = 0;
	CheckResult(xrEnumerateInstanceExtensionProperties(nullptr, 0, &propertyCount, nullptr));

	std::vector<XrExtensionProperties> properties;
	properties.resize(propertyCount, XrExtensionProperties { XR_TYPE_EXTENSION_PROPERTIES });

	CheckResult(xrEnumerateInstanceExtensionProperties(nullptr, properties.size(), &propertyCount, properties.data()));

	for (const auto &prop : properties) {
		if (std::strncmp(prop.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, XR_MAX_EXTENSION_NAME_SIZE) == 0) {
			extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
			m_cylinderSupported = true;
		}
	}

	XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(instanceInfo.applicationInfo.applicationName, "Pioneer", XR_MAX_APPLICATION_NAME_SIZE - 1);
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.enabledExtensionNames = extensions.data();
	instanceInfo.enabledExtensionCount = extensions.size();

	if (CheckResult(xrCreateInstance(&instanceInfo, &m_xrInstance))) {
		return false;
	}

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	if (CheckResult(xrGetSystem(m_xrInstance, &systemInfo, &m_xrSystemID))) {
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
	if (eglGetCurrentContext()) {
		XrGraphicsBindingEGLMNDX graphicsBindingGL = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
		graphicsBindingGL.context = eglGetCurrentContext();
		graphicsBindingGL.display = eglGetCurrentDisplay();
		graphicsBindingGL.getProcAddress = eglGetProcAddress;

		EGLint configCount;
		eglGetConfigs(eglGetCurrentDisplay(), &graphicsBindingGL.config, 1, &configCount);

		sessionInfo.next = &graphicsBindingGL;
	} else {
		auto *display = XOpenDisplay(nullptr);
		int fbConfigCount;
		auto *fbConfigs = glXGetFBConfigs(display, 0, &fbConfigCount);

		XrGraphicsBindingOpenGLXlibKHR graphicsBindingGL = {XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};

		graphicsBindingGL.xDisplay = XOpenDisplay(nullptr);
		graphicsBindingGL.glxDrawable = glXGetCurrentDrawable();
		graphicsBindingGL.glxContext = glXGetCurrentContext();

		// not actually used but monado complains otherwise
		graphicsBindingGL.glxFBConfig = fbConfigs[0];
		graphicsBindingGL.visualid = 1;

		sessionInfo.next = &graphicsBindingGL;
	}
	#endif

	if (CheckResult(xrCreateSession(m_xrInstance, &sessionInfo, &m_xrSession))) {
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

	if (CheckResult(xrCreateSwapchain(m_xrSession, &swapchainInfo, &m_colorSwapchains[TARGET_HUD]))) {
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
	CheckResult(xrLocateViews(m_xrSession, &viewLocateInfo, &m_viewState, 2, &dummy, m_views));

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

		static const std::array<uint8_t, 4> clearColor = {0, 0, 0, 0};

		// FIXME: do this on the renderer instead of directly
		glClearTexImage(
			m_colorImages[i][colorImageIndex].image,
			0,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			clearColor.data()
		);

		m_renderTexturesColor[i].SetTextureID(m_colorImages[i][colorImageIndex].image);
		m_renderTargets[i]->SetColorTexture(&m_renderTexturesColor[i]);
	}
}

void VRSystem::DrawDesktopMirror() {
	// copy to the desktop window
	auto desktop_rt = Pi::GetApp()->GetRenderTarget();

	m_renderer->SetRenderTarget(desktop_rt);
	m_renderer->SetViewport({0, 0, m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight()});
	m_renderer->ClearScreen();
	m_renderer->BeginFrame();

	auto right_desc = m_renderTargets[TARGET_RIGHT]->GetDesc();

	auto ww = m_renderer->GetWindowWidth();
	auto wh = m_renderer->GetWindowHeight();

	auto w = std::min(ww, (int)right_desc.width);
	auto h = std::min(wh, (int)right_desc.height);
	auto x = ((int)right_desc.width - ww) / 2;
	auto y = ((int)right_desc.height - wh) / 2;

	m_renderer->CopyRenderTarget(
		m_renderTargets[TARGET_RIGHT],
		desktop_rt,
		{x, y, w, h},
		{0, 0, ww, wh}
	);

	/*m_renderer->CopyRenderTarget(
		m_renderTargets[TARGET_HUD],
		desktop_rt,
		{0, 0, VR::hudResolution[0], VR::hudResolution[1]},
		{0, 0, VR::hudResolution[0], VR::hudResolution[1]}
	);*/

	m_renderer->FlushCommandBuffers();
	m_renderer->EndFrame();
	m_renderer->SwapBuffers();
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
		reinterpret_cast<XrCompositionLayerBaseHeader *>(&m_hudLayerViewQuad),
	};

	if (m_cylinderSupported) {
		layers[1] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&m_hudLayerViewCylinder);
	}

	XrFrameEndInfo frameEndInfo = {
		XR_TYPE_FRAME_END_INFO,
		nullptr,
		m_frameState.predictedDisplayTime,
		XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		ShouldRender() ? 2u : 0u,
		layers
	};

	CheckResult(xrEndFrame(m_xrSession, &frameEndInfo));
}

bool VRSystem::ShouldRender() const {
	//if (!m_frameState.shouldRender) { return false; }
	if (!m_readyToRender) { return false; }
	return true;
}

void VRSystem::Update() {
	XrEventDataBuffer eventData{XR_TYPE_EVENT_DATA_BUFFER};
	XrResult pollResult = CheckResult(xrPollEvent(m_xrInstance, &eventData));

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

					CheckResult(xrBeginSession(m_xrSession, &beginInfo));
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
	auto viewport = Graphics::ViewportExtents(
		m_projectionLayerViews[eye].subImage.imageRect.offset.x,
		m_projectionLayerViews[eye].subImage.imageRect.offset.y,
		m_projectionLayerViews[eye].subImage.imageRect.extent.width,
		m_projectionLayerViews[eye].subImage.imageRect.extent.height
	);
	renderer.SetRenderTarget(m_renderTargets[eye]);
	renderer.SetViewport(viewport);
}

void VRSystem::SetupRenderingForHUD() {
	auto &renderer = *static_cast<Graphics::RendererOGL *>(m_renderer);
	auto viewport = Graphics::ViewportExtents(
		0, 0, VR::hudResolution[0], VR::hudResolution[1]
	);
	renderer.SetRenderTarget(m_renderTargets[TARGET_HUD]);
	renderer.SetViewport(viewport);
}
