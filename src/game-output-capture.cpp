/*
Game Output Capture
Copyright (C) 2026 Copilot

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <graphics/vec4.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <obs-module.h>
#include <obs.h>
#include <obs-hotkey.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/windows/window-helpers.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace {

#define SETTING_MODE "capture_mode"
#define SETTING_CAPTURE_BACKEND "capture_backend"
#define SETTING_WINDOW "window"
#define SETTING_WINDOW_PRIORITY "priority"
#define SETTING_CURSOR "capture_cursor"

constexpr char kModeAny[] = "any_fullscreen";
constexpr char kModeWindow[] = "window";
constexpr char kBackendGameCapture[] = "game_capture";
constexpr char kBackendWindowDisplay[] = "window_display";
constexpr char kBackendWindowDXGI[] = "window_dxgi";
constexpr char kBackendDesktopDuplication[] = "desktop_duplication";
constexpr char kWindowDisplayFrameSourceId[] = "game_output_capture_window_display_frame";
constexpr char kBackendToggleHotkeyName[] = "GameOutputCapture.ToggleCaptureBackend";
constexpr uint32_t kFallbackWidth = 1280;
constexpr uint32_t kFallbackHeight = 720;
constexpr uint32_t kMaxCaptureDimension = 8192;
constexpr uint64_t kMaxCaptureBytes = 512ULL * 1024 * 1024;
constexpr uint64_t kForegroundDebounceNs = 500'000'000ULL;

enum class CaptureBackend {
	GameCapture,
	WindowDisplay,
	DesktopDuplication,
};

CaptureBackend backend_from_setting(const char *value)
{
	if (!value)
		return CaptureBackend::GameCapture;
	if (strcmp(value, kBackendWindowDXGI) == 0 ||
	    strcmp(value, kBackendDesktopDuplication) == 0)
		return CaptureBackend::DesktopDuplication;
	if (strcmp(value, kBackendWindowDisplay) == 0)
		return CaptureBackend::WindowDisplay;
	if (strcmp(value, "window_only") == 0)
		return CaptureBackend::DesktopDuplication;
	return CaptureBackend::GameCapture;
}

const char *backend_setting_value(CaptureBackend backend)
{
	switch (backend) {
	case CaptureBackend::DesktopDuplication:
		return kBackendWindowDXGI;
	case CaptureBackend::WindowDisplay:
		return kBackendWindowDisplay;
	default:
		return kBackendGameCapture;
	}
}

const char *backend_display_name(CaptureBackend backend)
{
	switch (backend) {
	case CaptureBackend::DesktopDuplication:
		return "Window and DXGI";
	case CaptureBackend::WindowDisplay:
		return "Window and Display";
	default:
		return "Game Capture";
	}
}

struct WindowDisplayFrameSource {
	obs_source_t *source = nullptr;
	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};
};

struct WindowInfo {
	HWND window = nullptr;
	HMONITOR monitor = nullptr;
	RECT windowRect{};
	RECT clientRect{};
	RECT outputRect{};
	uint32_t width = 0;
	uint32_t height = 0;
};

struct GameOutputCapture {
	obs_source_t *source = nullptr;
	obs_source_t *gameCaptureSource = nullptr;
	obs_source_t *windowDisplaySource = nullptr;
	obs_hotkey_id backendToggleHotkey = OBS_INVALID_HOTKEY_ID;
	std::atomic<CaptureBackend> backend{CaptureBackend::GameCapture};
	std::atomic<CaptureBackend> videoBackend{CaptureBackend::WindowDisplay};
	std::atomic<bool> sourceShowing{false};
	std::mutex backendMutex;
	std::atomic<DWORD> processId{0};
	std::atomic<bool> targetRunning{false};
	std::atomic<uint32_t> targetWidth{kFallbackWidth};
	std::atomic<uint32_t> targetHeight{kFallbackHeight};
	std::atomic<bool> specificWindowMode{true};
	std::atomic<bool> captureCursor{false};
	std::atomic<bool> gameCaptureShowing{false};
	std::atomic<bool> shuttingDown{false};
	std::mutex targetMutex;
	std::string windowMatch;
	std::atomic<int> windowPriority{WINDOW_PRIORITY_EXE};
	HANDLE processMonitorStopEvent = nullptr;
	std::thread processMonitorThread;
	HANDLE videoStopEvent = nullptr;
	std::thread videoThread;
	std::mutex audioControlMutex;
	std::mutex audioMutex;
	HANDLE audioStopEvent = nullptr;
	std::thread audioThread;
};

bool get_window_info(HWND window, DWORD processId, WindowInfo &info)
{
	DWORD windowProcessId = 0;
	GetWindowThreadProcessId(window, &windowProcessId);
	if (windowProcessId != processId || !IsWindowVisible(window) || IsIconic(window))
		return false;

	RECT client{};
	if (!GetClientRect(window, &client) || client.right <= client.left || client.bottom <= client.top)
		return false;

	RECT windowRect{};
	if (!GetWindowRect(window, &windowRect) || windowRect.right <= windowRect.left ||
	    windowRect.bottom <= windowRect.top)
		return false;

	POINT topLeft{client.left, client.top};
	POINT bottomRight{client.right, client.bottom};
	if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight))
		return false;

	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return false;

	RECT clientOnScreen{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
	const LONG width = clientOnScreen.right - clientOnScreen.left;
	const LONG height = clientOnScreen.bottom - clientOnScreen.top;
	if (width <= 0 || height <= 0 || width > static_cast<LONG>(kMaxCaptureDimension) ||
	    height > static_cast<LONG>(kMaxCaptureDimension))
		return false;

	info.window = window;
	info.monitor = monitor;
	info.windowRect = windowRect;
	info.clientRect = clientOnScreen;
	info.outputRect = {clientOnScreen.left - windowRect.left, clientOnScreen.top - windowRect.top,
			   clientOnScreen.right - windowRect.left, clientOnScreen.bottom - windowRect.top};
	info.width = static_cast<uint32_t>(width);
	info.height = static_cast<uint32_t>(height);
	return true;
}

struct TargetWindowSearch {
	DWORD processId = 0;
	HWND window = nullptr;
	uint64_t area = 0;
};

BOOL CALLBACK find_process_window(HWND window, LPARAM parameter)
{
	auto *search = reinterpret_cast<TargetWindowSearch *>(parameter);
	if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER))
		return TRUE;

	WindowInfo info{};
	if (get_window_info(window, search->processId, info)) {
		const uint64_t area = static_cast<uint64_t>(info.width) * info.height;
		if (area > search->area) {
			search->window = window;
			search->area = area;
		}
	}
	return TRUE;
}

bool get_process_window_target(DWORD processId, WindowInfo &info)
{
	if (!processId)
		return false;

	TargetWindowSearch search{processId};
	EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&search));
	return search.window && get_window_info(search.window, processId, info);
}

DWORD find_process_by_window_match(const std::string &windowMatch, int priority)
{
	char *windowClass = nullptr;
	char *title = nullptr;
	char *executable = nullptr;
	ms_build_window_strings(windowMatch.c_str(), &windowClass, &title, &executable);

	DWORD processId = 0;
	if (windowClass && title && executable && *executable) {
		const HWND window = ms_find_window(INCLUDE_MINIMIZED, static_cast<window_priority>(priority),
						   windowClass, title, executable);
		if (window)
			GetWindowThreadProcessId(window, &processId);
	}

	bfree(windowClass);
	bfree(title);
	bfree(executable);
	return processId;
}

bool get_window_match_dimensions(const std::string &windowMatch, uint32_t &width, uint32_t &height)
{
	char *windowClass = nullptr;
	char *title = nullptr;
	char *executable = nullptr;
	ms_build_window_strings(windowMatch.c_str(), &windowClass, &title, &executable);

	bool found = false;
	if (windowClass && title && executable && *executable) {
		const HWND window = ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_EXE, windowClass, title,
						   executable);
		RECT client{};
		if (window && GetClientRect(window, &client) && client.right > client.left &&
		    client.bottom > client.top) {
			width = std::clamp(static_cast<uint32_t>(client.right - client.left), 1U,
					   kMaxCaptureDimension);
			height = std::clamp(static_cast<uint32_t>(client.bottom - client.top), 1U,
					    kMaxCaptureDimension);
			found = true;
		}
	}

	bfree(windowClass);
	bfree(title);
	bfree(executable);
	return found;
}

void update_target_state(GameOutputCapture *capture, const std::string &windowMatch, bool specificWindowMode)
{
	capture->specificWindowMode.store(specificWindowMode);
	const DWORD targetProcessId =
		specificWindowMode ? find_process_by_window_match(windowMatch, WINDOW_PRIORITY_EXE) : 0;
	capture->targetRunning.store(targetProcessId != 0);
	if (targetProcessId == 0)
		return;

	uint32_t width = capture->targetWidth.load();
	uint32_t height = capture->targetHeight.load();
	if (get_window_match_dimensions(windowMatch, width, height)) {
		capture->targetWidth.store(width);
		capture->targetHeight.store(height);
	}
}

std::string migrate_legacy_window_match(obs_data_t *settings)
{
	const char *currentMatch = obs_data_get_string(settings, SETTING_WINDOW);
	if (currentMatch && *currentMatch)
		return currentMatch;

	const char *legacyExecutable = obs_data_get_string(settings, "application_executable");
	if (!legacyExecutable || !*legacyExecutable)
		return {};

	std::string executable(legacyExecutable);
	const size_t separator = executable.find_last_of("\\/");
	if (separator != std::string::npos)
		executable.erase(0, separator + 1);

	std::string encodedExecutable;
	encodedExecutable.reserve(executable.size());
	for (char character : executable) {
		if (character == '#')
			encodedExecutable += "#22";
		else
			encodedExecutable += character;
	}

	const std::string migratedMatch = "::" + encodedExecutable;
	obs_data_set_string(settings, SETTING_WINDOW, migratedMatch.c_str());
	obs_data_set_string(settings, "application_executable", "");
	obs_log(LOG_INFO, "Migrated the saved application selection to OBS window-match settings.");
	return migratedMatch;
}

obs_data_t *create_game_capture_settings(const char *mode, const char *windowMatch, int priority,
					 bool captureCursor)
{
	obs_data_t *settings = obs_data_create();
	if (!settings)
		return nullptr;

	obs_data_set_string(settings, SETTING_MODE, mode);
	obs_data_set_string(settings, SETTING_WINDOW, windowMatch);
	obs_data_set_int(settings, SETTING_WINDOW_PRIORITY, priority);
	obs_data_set_bool(settings, SETTING_CURSOR, captureCursor);
	obs_data_set_bool(settings, "capture_audio", false);
	obs_data_set_bool(settings, "limit_framerate", false);
	obs_data_set_bool(settings, "capture_overlays", false);
	obs_data_set_bool(settings, "anti_cheat_hook", true);
	obs_data_set_bool(settings, "sli_compatibility", false);
	obs_data_set_bool(settings, "allow_transparency", false);
	return settings;
}

bool is_process_foreground(DWORD processId)
{
	if (!processId)
		return false;

	DWORD foregroundProcessId = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcessId);
	return foregroundProcessId == processId;
}

bool create_output_device(HMONITOR monitor, ComPtr<ID3D11Device> &device)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return false;

	for (UINT adapterIndex = 0;; ++adapterIndex) {
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;

		for (UINT outputIndex = 0;; ++outputIndex) {
			ComPtr<IDXGIOutput> output;
			if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_OUTPUT_DESC outputDesc{};
			if (FAILED(output->GetDesc(&outputDesc)) || outputDesc.Monitor != monitor)
				continue;

			D3D_FEATURE_LEVEL featureLevel{};
			if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
						     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
						     &device, &featureLevel, nullptr)))
				return false;

			return true;
		}
	}

	return false;
}

HRESULT create_output_duplication(HMONITOR monitor, ComPtr<ID3D11Device> &device,
				  ComPtr<ID3D11DeviceContext> &context,
				  ComPtr<IDXGIOutputDuplication> &duplication,
				  const char *&failedStage, std::wstring &adapterName,
				  std::wstring &outputName)
{
	failedStage = "CreateDXGIFactory1";
	ComPtr<IDXGIFactory1> factory;
	HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(result))
		return result;

	for (UINT adapterIndex = 0;; ++adapterIndex) {
		ComPtr<IDXGIAdapter1> adapter;
		failedStage = "IDXGIFactory1::EnumAdapters1";
		result = factory->EnumAdapters1(adapterIndex, &adapter);
		if (result == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(result))
			return result;

		for (UINT outputIndex = 0;; ++outputIndex) {
			ComPtr<IDXGIOutput> output;
			failedStage = "IDXGIAdapter1::EnumOutputs";
			result = adapter->EnumOutputs(outputIndex, &output);
			if (result == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(result))
				return result;

			DXGI_OUTPUT_DESC outputDesc{};
			failedStage = "IDXGIOutput::GetDesc";
			result = output->GetDesc(&outputDesc);
			if (FAILED(result))
				return result;
			if (outputDesc.Monitor != monitor)
				continue;

			DXGI_ADAPTER_DESC1 adapterDesc{};
			failedStage = "IDXGIAdapter1::GetDesc1";
			result = adapter->GetDesc1(&adapterDesc);
			if (FAILED(result))
				return result;
			adapterName = adapterDesc.Description;
			outputName = outputDesc.DeviceName;

			D3D_FEATURE_LEVEL featureLevel{};
			failedStage = "D3D11CreateDevice";
			result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
						   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
						   D3D11_SDK_VERSION, &device, &featureLevel, &context);
			if (FAILED(result))
				return result;

			ComPtr<IDXGIOutput1> output1;
			failedStage = "IDXGIOutput::QueryInterface(IDXGIOutput1)";
			result = output.As(&output1);
			if (SUCCEEDED(result)) {
				failedStage = "IDXGIOutput1::DuplicateOutput";
				result = output1->DuplicateOutput(device.Get(), &duplication);
			}
			if (FAILED(result)) {
				device.Reset();
				context.Reset();
				return result;
			}

			return S_OK;
		}
	}

	failedStage = "matching the monitor to an adapter output";
	return DXGI_ERROR_NOT_FOUND;
}

void output_transparent_frame(GameOutputCapture *capture, uint32_t width, uint32_t height)
{
	if (!capture->windowDisplaySource)
		return;

	width = std::clamp(width, 1U, kMaxCaptureDimension);
	height = std::clamp(height, 1U, kMaxCaptureDimension);
	const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4;
	if (byteCount > kMaxCaptureBytes)
		return;

	std::vector<uint8_t> pixels(static_cast<size_t>(byteCount), 0);
	obs_source_frame frame{};
	frame.data[0] = pixels.data();
	frame.linesize[0] = width * 4;
	frame.width = width;
	frame.height = height;
	frame.timestamp = os_gettime_ns();
	frame.format = VIDEO_FORMAT_BGRA;
	frame.full_range = true;
	obs_source_output_video(capture->windowDisplaySource, &frame);
}

void output_captured_frame(GameOutputCapture *capture, uint8_t *pixels, uint32_t width, uint32_t height,
			   uint32_t rowPitch, uint64_t timestamp, uint64_t &submissionTimeNs)
{
	if (!capture->windowDisplaySource)
		return;

	const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4;
	if (width == 0 || height == 0 || width > kMaxCaptureDimension || height > kMaxCaptureDimension ||
	    rowPitch < width * 4 || byteCount > kMaxCaptureBytes)
		return;

	std::vector<uint8_t> opaquePixels(static_cast<size_t>(byteCount));
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *sourceRow = pixels + static_cast<size_t>(y) * rowPitch;
		uint8_t *outputRow = opaquePixels.data() + static_cast<size_t>(y) * width * 4;
		std::memcpy(outputRow, sourceRow, static_cast<size_t>(width) * 4);
		for (uint32_t x = 0; x < width; ++x)
			outputRow[static_cast<size_t>(x) * 4 + 3] = 0xFF;
	}

	obs_source_frame frame{};
	frame.data[0] = opaquePixels.data();
	frame.linesize[0] = width * 4;
	frame.width = width;
	frame.height = height;
	frame.timestamp = timestamp;
	frame.format = VIDEO_FORMAT_BGRA;
	frame.full_range = true;
	submissionTimeNs = os_gettime_ns();
	// libobs copies the frame data before obs_source_output_video returns.
	obs_source_output_video(capture->windowDisplaySource, &frame);
}

void set_capture_cursor(const winrt::Windows::Graphics::Capture::GraphicsCaptureSession &session,
			bool enabled, const char *capturePath)
{
	if (!session)
		return;
	try {
		session.IsCursorCaptureEnabled(enabled);
	} catch (const winrt::hresult_error &error) {
		obs_log(LOG_WARNING, "Could not %s cursor capture for the %s path (0x%08lX).",
			enabled ? "enable" : "disable", capturePath,
			static_cast<unsigned long>(error.code()));
	}
}

void composite_desktop_duplication_cursor(std::vector<uint8_t> &pixels, uint32_t width,
					  uint32_t height, uint32_t rowPitch,
					  const std::vector<uint8_t> &shape,
					  const DXGI_OUTDUPL_POINTER_SHAPE_INFO &shapeInfo,
					  POINT pointerPosition, const RECT &monitorRect, const RECT &crop,
					  uint32_t desktopWidth, uint32_t desktopHeight);

void video_capture_loop_impl(GameOutputCapture *capture, bool useDxgiForeground)
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	using namespace winrt::Windows::Graphics::Capture;
	using namespace winrt::Windows::Graphics::DirectX;
	using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> staging;
	ComPtr<ID3D11Device> dxgiDevice;
	ComPtr<ID3D11DeviceContext> dxgiContext;
	ComPtr<IDXGIOutputDuplication> dxgiDuplication;
	ComPtr<ID3D11Texture2D> dxgiStaging;
	std::vector<uint8_t> dxgiPointerShape;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO dxgiPointerShapeInfo{};
	POINT dxgiPointerPosition{};
	bool dxgiPointerVisible = false;
	GraphicsCaptureItem captureItem{nullptr};
	Direct3D11CaptureFramePool framePool{nullptr};
	GraphicsCaptureSession session{nullptr};
	GraphicsCaptureItem monitorCaptureItem{nullptr};
	Direct3D11CaptureFramePool monitorFramePool{nullptr};
	GraphicsCaptureSession monitorSession{nullptr};
	HANDLE frameArrivedEvent = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
	if (!frameArrivedEvent) {
		winrt::uninit_apartment();
		obs_log(LOG_ERROR, "Could not create Windows Graphics Capture frame semaphore.");
		return;
	}
	winrt::event_token frameArrivedToken{};
	winrt::event_token monitorFrameArrivedToken{};
	HMONITOR currentMonitor = nullptr;
	RECT currentMonitorRect{};
	uint32_t lastWidth = kFallbackWidth;
	uint32_t lastHeight = kFallbackHeight;
	bool unavailableFrameSent = false;
	bool warnedAboutFrame = false;
	uint64_t capturedFrames = 0;
	uint64_t lastFpsLog = os_gettime_ns();
	uint64_t dxgiCapturedFrames = 0;
	uint64_t lastDxgiFpsLogNs = lastFpsLog;
	uint64_t outputFrameIntervalNs = 0;
	uint64_t nextDueNs = 0;
	uint64_t pacingDroppedFrames = 0;
	uint64_t lastVideoInfoCheckNs = 0;
	uint64_t lastSubmittedFrameTimestampNs = 0;
	bool warnedAboutVideoInfo = false;
	uint64_t diagnosticFrames = 0;
	uint64_t validTimestampFrames = 0;
	uint64_t sourceIntervalCount = 0;
	uint64_t sourceIntervalSumNs = 0;
	uint64_t sourceIntervalMinNs = UINT64_MAX;
	uint64_t sourceIntervalMaxNs = 0;
	int64_t dequeueAgeSumNs = 0;
	int64_t dequeueAgeMinNs = INT64_MAX;
	int64_t dequeueAgeMaxNs = INT64_MIN;
	int64_t submissionAgeSumNs = 0;
	int64_t submissionAgeMinNs = INT64_MAX;
	int64_t submissionAgeMaxNs = INT64_MIN;
	uint64_t dequeueToSubmissionSumNs = 0;
	uint64_t dequeueToSubmissionMaxNs = 0;
	uint64_t previousFrameTimestampNs = 0;
	uint64_t lastTimingLogNs = lastFpsLog;
	HWND currentWindow = nullptr;
	RECT currentWindowRect{};
	uint64_t foregroundSinceNs = 0;
	uint64_t lastTargetSearchNs = 0;
	bool usingMonitorCapture = false;
	bool monitorCaptureAvailable = false;
	bool monitorCaptureAttempted = false;
	bool haveLoggedCaptureMode = false;
	bool lastLoggedMonitorMode = false;
	bool windowCursorSettingKnown = false;
	bool monitorCursorSettingKnown = false;
	bool windowCursorSetting = false;
	bool monitorCursorSetting = false;

	auto clear_capture_sessions = [&] {
		if (framePool && frameArrivedToken.value)
			framePool.FrameArrived(frameArrivedToken);
		if (monitorFramePool && monitorFrameArrivedToken.value)
			monitorFramePool.FrameArrived(monitorFrameArrivedToken);
		frameArrivedToken = {};
		monitorFrameArrivedToken = {};
		session = nullptr;
		monitorSession = nullptr;
		framePool = nullptr;
		monitorFramePool = nullptr;
		captureItem = nullptr;
		monitorCaptureItem = nullptr;
		windowCursorSettingKnown = false;
		monitorCursorSettingKnown = false;
	};

	auto drain_frame_notifications = [&] {
		DWORD waitResult = WAIT_OBJECT_0;
		while (waitResult == WAIT_OBJECT_0)
			waitResult = WaitForSingleObject(frameArrivedEvent, 0);
		if (waitResult == WAIT_FAILED)
			obs_log(LOG_WARNING, "Could not drain capture-frame notifications (error %lu).",
				GetLastError());
	};

	auto stop_monitor_capture = [&] {
		if (monitorFramePool && monitorFrameArrivedToken.value)
			monitorFramePool.FrameArrived(monitorFrameArrivedToken);
		monitorFrameArrivedToken = {};
		monitorSession = nullptr;
		monitorFramePool = nullptr;
		monitorCaptureItem = nullptr;
		monitorCaptureAvailable = false;
		monitorCursorSettingKnown = false;
	};

	auto start_monitor_capture = [&] {
		try {
			ComPtr<IDXGIDevice> dxgiDevice;
			winrt::check_hresult(device.As(&dxgiDevice));
			winrt::com_ptr<IInspectable> inspectableDevice;
			winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
				dxgiDevice.Get(), inspectableDevice.put()));
			auto captureDevice = inspectableDevice.as<IDirect3DDevice>();
			auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
								     IGraphicsCaptureItemInterop>();
			winrt::check_hresult(interop->CreateForMonitor(
				currentMonitor, winrt::guid_of<GraphicsCaptureItem>(),
				winrt::put_abi(monitorCaptureItem)));
			const auto monitorSize = monitorCaptureItem.Size();
			monitorFramePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
				captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
				monitorSize);
			monitorFrameArrivedToken = monitorFramePool.FrameArrived(
				[frameArrivedEvent](Direct3D11CaptureFramePool const &,
						    winrt::Windows::Foundation::IInspectable const &) {
					if (!ReleaseSemaphore(frameArrivedEvent, 1, nullptr))
						obs_log(LOG_WARNING,
							"Could not queue a monitor-capture frame notification "
							"(error %lu).",
							GetLastError());
				});
			monitorSession = monitorFramePool.CreateCaptureSession(monitorCaptureItem);
			monitorCursorSetting = capture->captureCursor.load();
			set_capture_cursor(monitorSession, monitorCursorSetting, "monitor");
			monitorCursorSettingKnown = true;
			try {
				monitorSession.as<IGraphicsCaptureSession3>().IsBorderRequired(false);
			} catch (const winrt::hresult_error &error) {
				obs_log(LOG_WARNING,
					"Windows does not allow disabling the monitor capture border "
					"(0x%08lX).",
					static_cast<unsigned long>(error.code()));
			}
			monitorSession.StartCapture();
			monitorCaptureAvailable = true;
			obs_log(LOG_INFO, "Foreground monitor capture started for monitor %p.",
				currentMonitor);
			return true;
		} catch (const winrt::hresult_error &error) {
			stop_monitor_capture();
			obs_log(LOG_WARNING,
				"Could not start foreground monitor capture; continuing with window capture "
				"(0x%08lX).",
				static_cast<unsigned long>(error.code()));
			return false;
		}
	};

	while (WaitForSingleObject(capture->videoStopEvent, 0) != WAIT_OBJECT_0) {
		WindowInfo target{};
		const DWORD processId = capture->processId.load();
		const uint64_t targetCheckTimeNs = os_gettime_ns();
		bool targetAvailable = currentWindow &&
				       get_window_info(currentWindow, processId, target);
		if (!targetAvailable &&
		    (lastTargetSearchNs == 0 ||
		     targetCheckTimeNs - lastTargetSearchNs >= 100'000'000ULL)) {
			targetAvailable = get_process_window_target(processId, target);
			lastTargetSearchNs = targetCheckTimeNs;
		}
		if (!targetAvailable) {
			clear_capture_sessions();
			staging.Reset();
			context.Reset();
			device.Reset();
			dxgiDuplication.Reset();
			dxgiStaging.Reset();
			dxgiContext.Reset();
			dxgiDevice.Reset();
			dxgiPointerShape.clear();
			dxgiPointerVisible = false;
			currentWindow = nullptr;
			currentMonitor = nullptr;
			foregroundSinceNs = 0;
			usingMonitorCapture = false;
			monitorCaptureAvailable = false;
			monitorCaptureAttempted = false;
			if (!unavailableFrameSent) {
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
			}
			WaitForSingleObject(capture->videoStopEvent, 50);
			continue;
		}

		lastWidth = target.width;
		lastHeight = target.height;
		if (target.window != currentWindow || target.monitor != currentMonitor ||
		    target.windowRect.right - target.windowRect.left !=
			    currentWindowRect.right - currentWindowRect.left ||
		    target.windowRect.bottom - target.windowRect.top !=
			    currentWindowRect.bottom - currentWindowRect.top) {
			clear_capture_sessions();
			staging.Reset();
			context.Reset();
			device.Reset();
			dxgiDuplication.Reset();
			dxgiStaging.Reset();
			dxgiContext.Reset();
			dxgiDevice.Reset();
			dxgiPointerShape.clear();
			dxgiPointerVisible = false;
			currentWindow = target.window;
			currentWindowRect = target.windowRect;
			currentMonitor = target.monitor;
			foregroundSinceNs = 0;
			usingMonitorCapture = false;
			monitorCaptureAvailable = false;
			monitorCaptureAttempted = false;
			MONITORINFO monitorInfo{sizeof(monitorInfo)};
			if (!GetMonitorInfoW(currentMonitor, &monitorInfo)) {
				currentMonitor = nullptr;
				obs_log(LOG_WARNING,
					"Could not determine the selected application's monitor bounds (error %lu).",
					GetLastError());
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
				WaitForSingleObject(capture->videoStopEvent, 1000);
				continue;
			}
			currentMonitorRect = monitorInfo.rcMonitor;
			if (!create_output_device(currentMonitor, device)) {
				currentMonitor = nullptr;
				obs_log(LOG_WARNING,
					"Could not create a D3D11 device for the selected application's display.");
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
				WaitForSingleObject(capture->videoStopEvent, 1000);
				continue;
			}
			device->GetImmediateContext(&context);

			try {
				ComPtr<IDXGIDevice> dxgiDevice;
				winrt::check_hresult(device.As(&dxgiDevice));
				winrt::com_ptr<IInspectable> inspectableDevice;
				winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
											  inspectableDevice.put()));
				auto captureDevice = inspectableDevice.as<IDirect3DDevice>();
				auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
									     IGraphicsCaptureItemInterop>();
				winrt::check_hresult(interop->CreateForWindow(target.window,
									      winrt::guid_of<GraphicsCaptureItem>(),
									      winrt::put_abi(captureItem)));
				const auto size = captureItem.Size();
				framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
					captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, size);
				frameArrivedToken = framePool.FrameArrived([frameArrivedEvent](
										   Direct3D11CaptureFramePool const &,
										   winrt::Windows::Foundation::
											   IInspectable const &) {
					if (!ReleaseSemaphore(frameArrivedEvent, 1, nullptr))
						obs_log(LOG_WARNING,
							"Could not queue a Windows Graphics Capture frame notification "
							"(error %lu).",
							GetLastError());
				});
				session = framePool.CreateCaptureSession(captureItem);
				windowCursorSetting = capture->captureCursor.load();
				set_capture_cursor(session, windowCursorSetting, "window");
				windowCursorSettingKnown = true;
				try {
					session.as<IGraphicsCaptureSession3>().IsBorderRequired(false);
				} catch (const winrt::hresult_error &error) {
					obs_log(LOG_WARNING,
						"Windows does not allow disabling the capture border (0x%08lX).",
						static_cast<unsigned long>(error.code()));
				}
				session.StartCapture();
				obs_log(LOG_INFO, "Windows Graphics Capture started for selected window %p.",
					target.window);
			} catch (const winrt::hresult_error &error) {
				obs_log(LOG_WARNING,
					"Could not start Windows Graphics Capture for the selected window "
					"(0x%08lX).",
					static_cast<unsigned long>(error.code()));
				clear_capture_sessions();
				device.Reset();
				context.Reset();
				currentMonitor = nullptr;
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
				WaitForSingleObject(capture->videoStopEvent, 1000);
				continue;
			}
		}

		const uint64_t focusCheckTimeNs = os_gettime_ns();
		const bool processIsForeground = is_process_foreground(processId);
		bool focusDebounced = false;
		if (processIsForeground) {
			if (foregroundSinceNs == 0)
				foregroundSinceNs = focusCheckTimeNs;
			focusDebounced =
				focusCheckTimeNs - foregroundSinceNs >= kForegroundDebounceNs;
			if (!monitorCaptureAttempted && (!useDxgiForeground || focusDebounced)) {
				monitorCaptureAttempted = true;
				if (useDxgiForeground) {
					const char *failedStage = "unknown";
					std::wstring adapterName;
					std::wstring outputName;
					const HRESULT result = create_output_duplication(
						currentMonitor, dxgiDevice, dxgiContext, dxgiDuplication,
						failedStage, adapterName, outputName);
					if (FAILED(result)) {
						obs_log(LOG_WARNING,
							"Could not start foreground DXGI capture for monitor %p "
							"(stage: %s, adapter: %ls, output: %ls, HRESULT: 0x%08lX); "
							"continuing with selected-window capture.",
							currentMonitor, failedStage,
							adapterName.empty() ? L"<unknown>" : adapterName.c_str(),
							outputName.empty() ? L"<unknown>" : outputName.c_str(),
							static_cast<unsigned long>(result));
						dxgiDuplication.Reset();
						dxgiContext.Reset();
						dxgiDevice.Reset();
					} else {
						obs_log(LOG_INFO,
							"Foreground DXGI capture started after the selected "
							"application remained focused for 500 ms.");
					}
				} else {
					start_monitor_capture();
				}
			}
			usingMonitorCapture = useDxgiForeground ?
						      dxgiDuplication && focusDebounced :
						      monitorCaptureAvailable && focusDebounced;
		} else {
			foregroundSinceNs = 0;
			usingMonitorCapture = false;
			monitorCaptureAttempted = false;
			dxgiDuplication.Reset();
			dxgiStaging.Reset();
			dxgiContext.Reset();
			dxgiDevice.Reset();
			dxgiPointerShape.clear();
			dxgiPointerVisible = false;
			if (monitorCaptureAvailable)
				stop_monitor_capture();
		}
		const bool captureCursor = capture->captureCursor.load();
		if (session && (!windowCursorSettingKnown || windowCursorSetting != captureCursor)) {
			set_capture_cursor(session, captureCursor, "window");
			windowCursorSetting = captureCursor;
			windowCursorSettingKnown = true;
		}
		if (monitorSession &&
		    (!monitorCursorSettingKnown || monitorCursorSetting != captureCursor)) {
			set_capture_cursor(monitorSession, captureCursor, "monitor");
			monitorCursorSetting = captureCursor;
			monitorCursorSettingKnown = true;
		}
		if (!haveLoggedCaptureMode || usingMonitorCapture != lastLoggedMonitorMode) {
			const char *capturePath =
				usingMonitorCapture ?
					(useDxgiForeground ? "foreground DXGI crop" : "foreground monitor crop") :
					"application window";
			obs_log(LOG_INFO, "Active video capture path: %s.", capturePath);
			haveLoggedCaptureMode = true;
			lastLoggedMonitorMode = usingMonitorCapture;
			capturedFrames = 0;
			lastFpsLog = focusCheckTimeNs;
			diagnosticFrames = 0;
			validTimestampFrames = 0;
			sourceIntervalCount = 0;
			sourceIntervalSumNs = 0;
			sourceIntervalMinNs = UINT64_MAX;
			sourceIntervalMaxNs = 0;
			dequeueAgeSumNs = 0;
			dequeueAgeMinNs = INT64_MAX;
			dequeueAgeMaxNs = INT64_MIN;
			submissionAgeSumNs = 0;
			submissionAgeMinNs = INT64_MAX;
			submissionAgeMaxNs = INT64_MIN;
			dequeueToSubmissionSumNs = 0;
			dequeueToSubmissionMaxNs = 0;
			previousFrameTimestampNs = 0;
			lastTimingLogNs = focusCheckTimeNs;
		}
		if (useDxgiForeground && usingMonitorCapture) {
			drain_frame_notifications();
			DXGI_OUTDUPL_FRAME_INFO dxgiFrameInfo{};
			ComPtr<IDXGIResource> dxgiResource;
			const HRESULT acquireResult = dxgiDuplication->AcquireNextFrame(
				50, &dxgiFrameInfo, dxgiResource.GetAddressOf());
			if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT)
				continue;
			if (acquireResult == DXGI_ERROR_ACCESS_LOST || FAILED(acquireResult)) {
				obs_log(LOG_WARNING,
					"Foreground DXGI capture lost access to its output (0x%08lX); "
					"returning to selected-window capture and retrying.",
					static_cast<unsigned long>(acquireResult));
				dxgiDuplication.Reset();
				dxgiStaging.Reset();
				dxgiContext.Reset();
				dxgiDevice.Reset();
				monitorCaptureAttempted = false;
				continue;
			}

			struct DxgiFrameRelease {
				IDXGIOutputDuplication *duplication;
				~DxgiFrameRelease() { duplication->ReleaseFrame(); }
			} dxgiFrameRelease{dxgiDuplication.Get()};

			if (dxgiFrameInfo.LastMouseUpdateTime.QuadPart != 0) {
				dxgiPointerPosition = dxgiFrameInfo.PointerPosition.Position;
				dxgiPointerVisible = dxgiFrameInfo.PointerPosition.Visible != FALSE;
			}
			if (dxgiFrameInfo.PointerShapeBufferSize > 0) {
				dxgiPointerShape.resize(dxgiFrameInfo.PointerShapeBufferSize);
				UINT bufferSize = 0;
				const HRESULT shapeResult = dxgiDuplication->GetFramePointerShape(
					dxgiFrameInfo.PointerShapeBufferSize, dxgiPointerShape.data(),
					&bufferSize, &dxgiPointerShapeInfo);
				if (SUCCEEDED(shapeResult))
					dxgiPointerShape.resize(bufferSize);
				else
					dxgiPointerShape.clear();
			}

			ComPtr<ID3D11Texture2D> desktopTexture;
			HRESULT result = dxgiResource.As(&desktopTexture);
			D3D11_TEXTURE2D_DESC desktopDesc{};
			if (SUCCEEDED(result))
				desktopTexture->GetDesc(&desktopDesc);
			const LONG monitorWidth = currentMonitorRect.right - currentMonitorRect.left;
			const LONG monitorHeight = currentMonitorRect.bottom - currentMonitorRect.top;
			if (FAILED(result) || !desktopDesc.Width || !desktopDesc.Height ||
			    monitorWidth <= 0 || monitorHeight <= 0)
				continue;

			const double scaleX = desktopDesc.Width / static_cast<double>(monitorWidth);
			const double scaleY = desktopDesc.Height / static_cast<double>(monitorHeight);
			RECT crop{
				static_cast<LONG>(std::floor(
					(target.clientRect.left - currentMonitorRect.left) * scaleX)),
				static_cast<LONG>(std::floor(
					(target.clientRect.top - currentMonitorRect.top) * scaleY)),
				static_cast<LONG>(std::ceil(
					(target.clientRect.right - currentMonitorRect.left) * scaleX)),
				static_cast<LONG>(std::ceil(
					(target.clientRect.bottom - currentMonitorRect.top) * scaleY)),
			};
			crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(desktopDesc.Width));
			crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(desktopDesc.Height));
			crop.right = std::clamp<LONG>(crop.right, 0, static_cast<LONG>(desktopDesc.Width));
			crop.bottom = std::clamp<LONG>(crop.bottom, 0, static_cast<LONG>(desktopDesc.Height));
			if (crop.right <= crop.left || crop.bottom <= crop.top)
				continue;

			const uint32_t width = static_cast<uint32_t>(crop.right - crop.left);
			const uint32_t height = static_cast<uint32_t>(crop.bottom - crop.top);
			const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4;
			if (width > kMaxCaptureDimension || height > kMaxCaptureDimension ||
			    byteCount > kMaxCaptureBytes)
				continue;

			D3D11_TEXTURE2D_DESC stagingDesc = desktopDesc;
			stagingDesc.Width = width;
			stagingDesc.Height = height;
			stagingDesc.MipLevels = 1;
			stagingDesc.ArraySize = 1;
			stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			stagingDesc.SampleDesc.Count = 1;
			stagingDesc.SampleDesc.Quality = 0;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.MiscFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			D3D11_TEXTURE2D_DESC previousDesc{};
			if (dxgiStaging)
				dxgiStaging->GetDesc(&previousDesc);
			if (!dxgiStaging || previousDesc.Width != width ||
			    previousDesc.Height != height || previousDesc.Format != stagingDesc.Format) {
				dxgiStaging.Reset();
				result = dxgiDevice->CreateTexture2D(&stagingDesc, nullptr, &dxgiStaging);
			}
			if (FAILED(result))
				continue;

			const D3D11_BOX region{static_cast<UINT>(crop.left), static_cast<UINT>(crop.top),
					       0, static_cast<UINT>(crop.right),
					       static_cast<UINT>(crop.bottom), 1};
			dxgiContext->CopySubresourceRegion(dxgiStaging.Get(), 0, 0, 0, 0,
							  desktopTexture.Get(), 0, &region);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			result = dxgiContext->Map(dxgiStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
			if (FAILED(result))
				continue;

			std::vector<uint8_t> framePixels(static_cast<size_t>(byteCount));
			for (uint32_t y = 0; y < height; ++y)
				std::memcpy(framePixels.data() + static_cast<size_t>(y) * width * 4,
					    static_cast<const uint8_t *>(mapped.pData) +
						    static_cast<size_t>(y) * mapped.RowPitch,
					    static_cast<size_t>(width) * 4);
			dxgiContext->Unmap(dxgiStaging.Get(), 0);
			if (captureCursor && dxgiPointerVisible)
				composite_desktop_duplication_cursor(
					framePixels, width, height, width * 4, dxgiPointerShape,
					dxgiPointerShapeInfo, dxgiPointerPosition, currentMonitorRect, crop,
					desktopDesc.Width, desktopDesc.Height);

			WindowInfo stillActive{};
			if (get_window_info(target.window, processId, stillActive) &&
			    is_process_foreground(processId) && stillActive.window == target.window &&
			    stillActive.monitor == target.monitor &&
			    stillActive.clientRect.left == target.clientRect.left &&
			    stillActive.clientRect.top == target.clientRect.top &&
			    stillActive.clientRect.right == target.clientRect.right &&
			    stillActive.clientRect.bottom == target.clientRect.bottom) {
				uint64_t submissionTimeNs = 0;
				output_captured_frame(capture, framePixels.data(), width, height, width * 4,
						      os_gettime_ns(), submissionTimeNs);
				++dxgiCapturedFrames;
				const uint64_t now = os_gettime_ns();
				if (now - lastDxgiFpsLogNs >= 5'000'000'000ULL) {
					obs_log(LOG_INFO, "DXGI foreground crop output rate: %.1f FPS.",
						static_cast<double>(dxgiCapturedFrames) * 1'000'000'000.0 /
							static_cast<double>(now - lastDxgiFpsLogNs));
					dxgiCapturedFrames = 0;
					lastDxgiFpsLogNs = now;
				}
				unavailableFrameSent = false;
			}
			continue;
		}
		if (lastVideoInfoCheckNs == 0 ||
		    focusCheckTimeNs - lastVideoInfoCheckNs >= 1'000'000'000ULL) {
			obs_video_info videoInfo{};
			if (obs_get_video_info(&videoInfo) && videoInfo.fps_num > 0 &&
			    videoInfo.fps_den > 0) {
				outputFrameIntervalNs =
					1'000'000'000ULL * videoInfo.fps_den / videoInfo.fps_num;
				warnedAboutVideoInfo = false;
			} else {
				if (!warnedAboutVideoInfo)
					obs_log(LOG_WARNING,
						"Could not read OBS output frame rate; capture is not being frame-rate "
						"limited.");
				warnedAboutVideoInfo = true;
				outputFrameIntervalNs = 0;
			}
			lastVideoInfoCheckNs = focusCheckTimeNs;
		}

		HANDLE waitHandles[]{capture->videoStopEvent, frameArrivedEvent};
		const DWORD frameWaitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 50);
		if (frameWaitResult == WAIT_OBJECT_0)
			break;
		if (frameWaitResult == WAIT_TIMEOUT)
			continue;
		if (frameWaitResult != WAIT_OBJECT_0 + 1) {
			obs_log(LOG_ERROR, "Waiting for a Windows Graphics Capture frame failed (error %lu).",
				GetLastError());
			break;
		}
		drain_frame_notifications();

		bool frameOutput = false;
		Direct3D11CaptureFrame frame{nullptr};
		try {
			auto takeLatestFrame = [](Direct3D11CaptureFramePool const &pool,
						  Direct3D11CaptureFrame &latest) {
				while (true) {
					Direct3D11CaptureFrame next = pool.TryGetNextFrame();
					if (!next)
						break;
					latest = std::move(next);
				}
			};
			if (monitorFramePool) {
				Direct3D11CaptureFrame discardedFrame{nullptr};
				if (usingMonitorCapture) {
					takeLatestFrame(framePool, discardedFrame);
					takeLatestFrame(monitorFramePool, frame);
				} else {
					takeLatestFrame(monitorFramePool, discardedFrame);
					takeLatestFrame(framePool, frame);
				}
			} else {
				takeLatestFrame(framePool, frame);
			}
		} catch (const winrt::hresult_error &error) {
			if (!warnedAboutFrame) {
				obs_log(LOG_WARNING, "Windows Graphics Capture stopped (0x%08lX); retrying.",
					static_cast<unsigned long>(error.code()));
				warnedAboutFrame = true;
			}
			clear_capture_sessions();
			staging.Reset();
			currentMonitor = nullptr;
			monitorCaptureAvailable = false;
			usingMonitorCapture = false;
			if (!unavailableFrameSent)
				output_transparent_frame(capture, lastWidth, lastHeight);
			unavailableFrameSent = true;
			WaitForSingleObject(capture->videoStopEvent, 100);
			continue;
		}
		const uint64_t dequeueTimeNs = os_gettime_ns();

		ComPtr<ID3D11Texture2D> desktopTexture;
		HRESULT result = S_OK;
		D3D11_TEXTURE2D_DESC desktopDesc{};
		if (frame) {
			const uint64_t timestampNs =
				static_cast<uint64_t>(frame.SystemRelativeTime().count()) * 100;
			if (outputFrameIntervalNs > 0 && lastSubmittedFrameTimestampNs > 0) {
				const uint64_t slackNs = outputFrameIntervalNs / 4;
				if (timestampNs <= lastSubmittedFrameTimestampNs ||
				    (nextDueNs > 0 && timestampNs + slackNs < nextDueNs)) {
					frame = nullptr;
					++pacingDroppedFrames;
				}
			}
		}

		if (frame) {
			try {
				auto surface = frame.Surface()
						       .as<::Windows::Graphics::DirectX::Direct3D11::
								   IDirect3DDxgiInterfaceAccess>();
				result = surface->GetInterface(IID_PPV_ARGS(&desktopTexture));
			} catch (const winrt::hresult_error &error) {
				result = static_cast<HRESULT>(error.code());
			}
			if (SUCCEEDED(result))
				desktopTexture->GetDesc(&desktopDesc);
		}

		RECT crop{};
		if (usingMonitorCapture) {
			const LONG monitorWidth = currentMonitorRect.right - currentMonitorRect.left;
			const LONG monitorHeight = currentMonitorRect.bottom - currentMonitorRect.top;
			if (monitorWidth > 0 && monitorHeight > 0) {
				const double scaleX = desktopDesc.Width / static_cast<double>(monitorWidth);
				const double scaleY = desktopDesc.Height / static_cast<double>(monitorHeight);
				const double left =
					static_cast<double>(target.clientRect.left - currentMonitorRect.left) * scaleX;
				const double top =
					static_cast<double>(target.clientRect.top - currentMonitorRect.top) * scaleY;
				const double right =
					static_cast<double>(target.clientRect.right - currentMonitorRect.left) * scaleX;
				const double bottom =
					static_cast<double>(target.clientRect.bottom - currentMonitorRect.top) * scaleY;
				crop = {static_cast<LONG>(std::floor(left)), static_cast<LONG>(std::floor(top)),
					static_cast<LONG>(std::ceil(right)), static_cast<LONG>(std::ceil(bottom))};
			}
		} else if (desktopDesc.Width > 0 && desktopDesc.Height > 0) {
			const double scaleX = desktopDesc.Width /
					      static_cast<double>(target.windowRect.right -
								  target.windowRect.left);
			const double scaleY = desktopDesc.Height /
					      static_cast<double>(target.windowRect.bottom -
								  target.windowRect.top);
			crop = {static_cast<LONG>(std::floor(target.outputRect.left * scaleX)),
				static_cast<LONG>(std::floor(target.outputRect.top * scaleY)),
				static_cast<LONG>(std::ceil(target.outputRect.right * scaleX)),
				static_cast<LONG>(std::ceil(target.outputRect.bottom * scaleY))};
		}
		if (frame && SUCCEEDED(result) && crop.left >= 0 && crop.top >= 0 &&
		    crop.right <= static_cast<LONG>(desktopDesc.Width) &&
		    crop.bottom <= static_cast<LONG>(desktopDesc.Height) && crop.right > crop.left &&
		    crop.bottom > crop.top) {
			const uint32_t width = static_cast<uint32_t>(crop.right - crop.left);
			const uint32_t height = static_cast<uint32_t>(crop.bottom - crop.top);
			D3D11_TEXTURE2D_DESC stagingDesc = desktopDesc;
			stagingDesc.Width = width;
			stagingDesc.Height = height;
			stagingDesc.MipLevels = 1;
			stagingDesc.ArraySize = 1;
			stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			stagingDesc.SampleDesc.Count = 1;
			stagingDesc.SampleDesc.Quality = 0;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.BindFlags = 0;
			stagingDesc.MiscFlags = 0;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			D3D11_TEXTURE2D_DESC previousDesc{};
			if (staging)
				staging->GetDesc(&previousDesc);
			if (!staging || previousDesc.Width != width || previousDesc.Height != height ||
			    previousDesc.Format != stagingDesc.Format) {
				staging.Reset();
				result = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
			}

			if (SUCCEEDED(result)) {
				D3D11_BOX region{static_cast<UINT>(crop.left),  static_cast<UINT>(crop.top),    0,
						 static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
				context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, desktopTexture.Get(), 0,
							       &region);
				D3D11_MAPPED_SUBRESOURCE mapped{};
				result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
				if (SUCCEEDED(result)) {
					WindowInfo stillActive{};
					if (get_window_info(target.window, processId, stillActive) &&
					    (!usingMonitorCapture || is_process_foreground(processId)) &&
					    stillActive.window == target.window &&
					    stillActive.monitor == target.monitor &&
					    stillActive.windowRect.left == target.windowRect.left &&
					    stillActive.windowRect.top == target.windowRect.top &&
					    stillActive.windowRect.right == target.windowRect.right &&
					    stillActive.windowRect.bottom == target.windowRect.bottom &&
					    stillActive.clientRect.left == target.clientRect.left &&
					    stillActive.clientRect.top == target.clientRect.top &&
					    stillActive.clientRect.right == target.clientRect.right &&
					    stillActive.clientRect.bottom == target.clientRect.bottom) {
						const uint64_t frameTimestampNs =
							static_cast<uint64_t>(frame.SystemRelativeTime().count()) * 100;
						uint64_t submissionTimeNs = 0;
						output_captured_frame(
							capture, static_cast<uint8_t *>(mapped.pData), width,
							height, mapped.RowPitch, frameTimestampNs, submissionTimeNs);
						lastSubmittedFrameTimestampNs = frameTimestampNs;
						if (outputFrameIntervalNs > 0)
							nextDueNs = (nextDueNs == 0 ||
								     frameTimestampNs > nextDueNs + outputFrameIntervalNs) ?
									    frameTimestampNs + outputFrameIntervalNs :
									    nextDueNs + outputFrameIntervalNs;
						if (frameTimestampNs > 0 && submissionTimeNs >= dequeueTimeNs) {
							const int64_t dequeueAgeNs =
								static_cast<int64_t>(dequeueTimeNs) -
								static_cast<int64_t>(frameTimestampNs);
							const int64_t submissionAgeNs =
								static_cast<int64_t>(submissionTimeNs) -
								static_cast<int64_t>(frameTimestampNs);
							++validTimestampFrames;
							dequeueAgeSumNs += dequeueAgeNs;
							dequeueAgeMinNs = std::min(dequeueAgeMinNs, dequeueAgeNs);
							dequeueAgeMaxNs = std::max(dequeueAgeMaxNs, dequeueAgeNs);
							submissionAgeSumNs += submissionAgeNs;
							submissionAgeMinNs =
								std::min(submissionAgeMinNs, submissionAgeNs);
							submissionAgeMaxNs =
								std::max(submissionAgeMaxNs, submissionAgeNs);
							dequeueToSubmissionSumNs += submissionTimeNs - dequeueTimeNs;
							dequeueToSubmissionMaxNs =
								std::max(dequeueToSubmissionMaxNs, submissionTimeNs - dequeueTimeNs);
							if (previousFrameTimestampNs > 0 &&
							    frameTimestampNs > previousFrameTimestampNs) {
								const uint64_t intervalNs =
									frameTimestampNs - previousFrameTimestampNs;
								sourceIntervalSumNs += intervalNs;
								++sourceIntervalCount;
								sourceIntervalMinNs =
									std::min(sourceIntervalMinNs, intervalNs);
								sourceIntervalMaxNs =
									std::max(sourceIntervalMaxNs, intervalNs);
							}
							previousFrameTimestampNs = frameTimestampNs;
						}
						++diagnosticFrames;
						unavailableFrameSent = false;
						frameOutput = true;
					} else if (!unavailableFrameSent) {
						output_transparent_frame(capture, lastWidth, lastHeight);
						unavailableFrameSent = true;
						frameOutput = true;
					}
					context->Unmap(staging.Get(), 0);
				}
			}
		}

		if (frame && (FAILED(result) || !frameOutput)) {
			if (!warnedAboutFrame && FAILED(result)) {
				obs_log(LOG_WARNING,
					"Could not copy the selected application from the composed display (0x%08lX).",
					result);
				warnedAboutFrame = true;
			}
			if (!unavailableFrameSent)
				output_transparent_frame(capture, lastWidth, lastHeight);
			unavailableFrameSent = true;
		}
		if (SUCCEEDED(result) && frameOutput) {
			warnedAboutFrame = false;
			++capturedFrames;
			const uint64_t now = os_gettime_ns();
			if (now - lastFpsLog >= 5'000'000'000ULL) {
				obs_log(LOG_INFO,
					"Windows Graphics Capture %s output rate: %.1f FPS.",
					usingMonitorCapture ? "monitor" : "window",
					static_cast<double>(capturedFrames) * 1'000'000'000.0 /
						static_cast<double>(now - lastFpsLog));
				capturedFrames = 0;
				lastFpsLog = now;
				obs_log(LOG_INFO,
					"WGC pacing: %llu frames skipped by output-rate pacing in the last interval.",
					static_cast<unsigned long long>(pacingDroppedFrames));
				pacingDroppedFrames = 0;
			}
			if (now - lastTimingLogNs >= 5'000'000'000ULL) {
				if (validTimestampFrames > 0) {
					obs_log(LOG_INFO,
						"WGC %s timing (5s): frames=%llu timestamped=%llu "
						"source_interval_ms avg/min/max=%.2f/%.2f/%.2f "
						"dequeue_age_ms avg/min/max=%.2f/%.2f/%.2f "
						"submit_age_ms avg/min/max=%.2f/%.2f/%.2f "
						"dequeue_to_submit_ms avg/max=%.2f/%.2f",
						usingMonitorCapture ? "monitor" : "window",
						static_cast<unsigned long long>(diagnosticFrames),
						static_cast<unsigned long long>(validTimestampFrames),
						sourceIntervalCount > 0
							? static_cast<double>(sourceIntervalSumNs) /
								  static_cast<double>(sourceIntervalCount) / 1'000'000.0
							: 0.0,
						sourceIntervalMinNs == UINT64_MAX
							? 0.0
							: static_cast<double>(sourceIntervalMinNs) / 1'000'000.0,
						static_cast<double>(sourceIntervalMaxNs) / 1'000'000.0,
						static_cast<double>(dequeueAgeSumNs) /
							static_cast<double>(validTimestampFrames) / 1'000'000.0,
						static_cast<double>(dequeueAgeMinNs) / 1'000'000.0,
						static_cast<double>(dequeueAgeMaxNs) / 1'000'000.0,
						static_cast<double>(submissionAgeSumNs) /
							static_cast<double>(validTimestampFrames) / 1'000'000.0,
						static_cast<double>(submissionAgeMinNs) / 1'000'000.0,
						static_cast<double>(submissionAgeMaxNs) / 1'000'000.0,
						static_cast<double>(dequeueToSubmissionSumNs) /
							static_cast<double>(validTimestampFrames) / 1'000'000.0,
						static_cast<double>(dequeueToSubmissionMaxNs) / 1'000'000.0);
				} else {
					obs_log(LOG_WARNING,
						"WGC timing (5s): frames=%llu but no valid frame timestamps were "
						"available.",
						static_cast<unsigned long long>(diagnosticFrames));
				}
				diagnosticFrames = 0;
				validTimestampFrames = 0;
				sourceIntervalCount = 0;
				sourceIntervalSumNs = 0;
				sourceIntervalMinNs = UINT64_MAX;
				sourceIntervalMaxNs = 0;
				dequeueAgeSumNs = 0;
				dequeueAgeMinNs = INT64_MAX;
				dequeueAgeMaxNs = INT64_MIN;
				submissionAgeSumNs = 0;
				submissionAgeMinNs = INT64_MAX;
				submissionAgeMaxNs = INT64_MIN;
				dequeueToSubmissionSumNs = 0;
				dequeueToSubmissionMaxNs = 0;
				lastTimingLogNs = now;
			}
		} else if (!frame) {
			WindowInfo stillActive{};
			if (!get_window_info(currentWindow, processId, stillActive) && !unavailableFrameSent) {
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
			}
		}
	}

	clear_capture_sessions();
	CloseHandle(frameArrivedEvent);
	staging.Reset();
	context.Reset();
	device.Reset();
	winrt::uninit_apartment();
}

void composite_desktop_duplication_cursor(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height,
					  uint32_t rowPitch, const std::vector<uint8_t> &shape,
					  const DXGI_OUTDUPL_POINTER_SHAPE_INFO &shapeInfo,
					  POINT pointerPosition, const RECT &monitorRect, const RECT &crop,
					  uint32_t desktopWidth, uint32_t desktopHeight)
{
	if (shape.empty() || !shapeInfo.Width || !shapeInfo.Height)
		return;

	const double scaleX = desktopWidth /
			      static_cast<double>(monitorRect.right - monitorRect.left);
	const double scaleY = desktopHeight /
			      static_cast<double>(monitorRect.bottom - monitorRect.top);
	const uint32_t shapeHeight =
		shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? shapeInfo.Height / 2 :
										      shapeInfo.Height;
	const LONG pointerLeft = pointerPosition.x - monitorRect.left -
				 static_cast<LONG>(std::lround(shapeInfo.HotSpot.x / scaleX));
	const LONG pointerTop = pointerPosition.y - monitorRect.top -
				static_cast<LONG>(std::lround(shapeInfo.HotSpot.y / scaleY));
	const LONG left = static_cast<LONG>(std::floor(pointerLeft * scaleX));
	const LONG top = static_cast<LONG>(std::floor(pointerTop * scaleY));
	const uint32_t outputWidth = pixels.empty() ? 0 : width;
	const uint32_t outputHeight = pixels.empty() ? 0 : height;

	for (uint32_t y = 0; y < outputHeight; ++y) {
		const LONG desktopY = crop.top + static_cast<LONG>(y);
		const LONG cursorY = desktopY - top;
		if (cursorY < 0 || cursorY >= static_cast<LONG>(std::ceil(shapeHeight * scaleY)))
			continue;
		const uint32_t sourceY = static_cast<uint32_t>(cursorY / scaleY);
		if (sourceY >= shapeHeight)
			continue;

		for (uint32_t x = 0; x < outputWidth; ++x) {
			const LONG desktopX = crop.left + static_cast<LONG>(x);
			const LONG cursorX = desktopX - left;
			if (cursorX < 0 || cursorX >= static_cast<LONG>(std::ceil(shapeInfo.Width * scaleX)))
				continue;
			const uint32_t sourceX = static_cast<uint32_t>(cursorX / scaleX);
			if (sourceX >= shapeInfo.Width)
				continue;

			uint8_t *destination = pixels.data() + static_cast<size_t>(y) * rowPitch +
					       static_cast<size_t>(x) * 4;
			if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
			    shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
				const size_t offset = static_cast<size_t>(sourceY) * shapeInfo.Pitch +
						      static_cast<size_t>(sourceX) * 4;
				if (offset + 4 > shape.size())
					return;
				const uint8_t *source = shape.data() + offset;
				if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR &&
				    source[3] == 0) {
					destination[0] ^= source[0];
					destination[1] ^= source[1];
					destination[2] ^= source[2];
				} else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
					std::memcpy(destination, source, 3);
				} else {
					const uint32_t alpha = source[3];
					const uint32_t inverseAlpha = 255 - alpha;
					for (size_t channel = 0; channel < 3; ++channel)
						destination[channel] = static_cast<uint8_t>(
							(source[channel] * alpha +
							 destination[channel] * inverseAlpha + 127) /
							255);
				}
				destination[3] = 0xFF;
			} else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
				const size_t byteIndex = sourceX / 8;
				const uint8_t bit = static_cast<uint8_t>(0x80 >> (sourceX % 8));
				const size_t andOffset = static_cast<size_t>(sourceY) * shapeInfo.Pitch + byteIndex;
				const size_t xorOffset = andOffset +
							 static_cast<size_t>(shapeInfo.Pitch) * shapeHeight;
				if (xorOffset >= shape.size() || andOffset >= shape.size())
					return;
				const bool andBit = (shape[andOffset] & bit) != 0;
				const bool xorBit = (shape[xorOffset] & bit) != 0;
				for (size_t channel = 0; channel < 3; ++channel) {
					if (!andBit)
						destination[channel] = xorBit ? 0xFF : 0;
					else if (xorBit)
						destination[channel] = static_cast<uint8_t>(~destination[channel]);
				}
				destination[3] = 0xFF;
			}
		}
	}
}

bool desktop_duplication_capture_loop(GameOutputCapture *capture)
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Texture2D> staging;
	HMONITOR currentMonitor = nullptr;
	HWND currentWindow = nullptr;
	RECT currentMonitorRect{};
	uint64_t lastTargetSearchNs = 0;
	uint32_t lastWidth = kFallbackWidth;
	uint32_t lastHeight = kFallbackHeight;
	bool unavailableFrameSent = false;
	bool warnedAboutInitialization = false;
	bool warnedAboutCapture = false;
	bool warnedAboutCursor = false;
	std::vector<uint8_t> pointerShape;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo{};
	POINT pointerPosition{};
	bool pointerVisible = false;

	while (WaitForSingleObject(capture->videoStopEvent, 0) != WAIT_OBJECT_0) {
		const DWORD processId = capture->processId.load();
		WindowInfo target{};
		const uint64_t now = os_gettime_ns();
		bool targetAvailable = currentWindow && get_window_info(currentWindow, processId, target);
		if (!targetAvailable &&
		    (lastTargetSearchNs == 0 || now - lastTargetSearchNs >= 100'000'000ULL)) {
			targetAvailable = get_process_window_target(processId, target);
			lastTargetSearchNs = now;
		}

		if (!targetAvailable) {
			duplication.Reset();
			staging.Reset();
			context.Reset();
			device.Reset();
			currentMonitor = nullptr;
			currentWindow = nullptr;
			if (!unavailableFrameSent) {
				output_transparent_frame(capture, lastWidth, lastHeight);
				unavailableFrameSent = true;
			}
			WaitForSingleObject(capture->videoStopEvent, 50);
			continue;
		}

		currentWindow = target.window;
		lastWidth = target.width;
		lastHeight = target.height;
		unavailableFrameSent = false;
		if (target.monitor != currentMonitor) {
			duplication.Reset();
			staging.Reset();
			context.Reset();
			device.Reset();
			pointerShape.clear();
			pointerVisible = false;
			warnedAboutInitialization = false;
			currentMonitor = target.monitor;
			MONITORINFO monitorInfo{sizeof(monitorInfo)};
			if (!GetMonitorInfoW(currentMonitor, &monitorInfo)) {
				obs_log(LOG_WARNING,
					"Could not determine the selected application's monitor bounds "
					"(error %lu).",
					GetLastError());
				currentMonitor = nullptr;
				continue;
			}
			currentMonitorRect = monitorInfo.rcMonitor;
		}

		if (!duplication) {
			const char *failedStage = "unknown";
			std::wstring adapterName;
			std::wstring outputName;
			const HRESULT initializeResult =
				create_output_duplication(currentMonitor, device, context, duplication,
							  failedStage, adapterName, outputName);
			if (FAILED(initializeResult)) {
				if (!warnedAboutInitialization) {
					obs_log(LOG_WARNING,
						"Could not start DXGI Desktop Duplication for monitor %p "
						"(stage: %s, adapter: %ls, output: %ls, HRESULT: 0x%08lX)%s.",
						currentMonitor,
						failedStage, adapterName.empty() ? L"<unknown>" : adapterName.c_str(),
						outputName.empty() ? L"<unknown>" : outputName.c_str(),
						static_cast<unsigned long>(initializeResult),
						initializeResult == DXGI_ERROR_UNSUPPORTED ?
							"; switching to Windows Graphics Capture" :
							"; retrying");
					warnedAboutInitialization = true;
				}
				if (initializeResult == DXGI_ERROR_UNSUPPORTED)
					return true;
				WaitForSingleObject(capture->videoStopEvent, 1000);
				continue;
			}
			warnedAboutInitialization = false;
			obs_log(LOG_INFO, "DXGI Desktop Duplication started for monitor %p.", currentMonitor);
		}

		DXGI_OUTDUPL_FRAME_INFO frameInfo{};
		ComPtr<IDXGIResource> resource;
		const HRESULT acquireResult =
			duplication->AcquireNextFrame(50, &frameInfo, resource.GetAddressOf());
		if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT)
			continue;
		if (acquireResult == DXGI_ERROR_ACCESS_LOST) {
			duplication.Reset();
			staging.Reset();
			context.Reset();
			device.Reset();
			WaitForSingleObject(capture->videoStopEvent, 100);
			continue;
		}
		if (FAILED(acquireResult)) {
			if (!warnedAboutCapture) {
				obs_log(LOG_WARNING,
					"DXGI Desktop Duplication could not acquire a frame (0x%08lX); "
					"reinitializing.",
					static_cast<unsigned long>(acquireResult));
				warnedAboutCapture = true;
			}
			duplication.Reset();
			staging.Reset();
			WaitForSingleObject(capture->videoStopEvent, 500);
			continue;
		}

		struct FrameRelease {
			IDXGIOutputDuplication *duplication;
			~FrameRelease() { duplication->ReleaseFrame(); }
		} frameRelease{duplication.Get()};
		warnedAboutCapture = false;

		if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
			pointerPosition = frameInfo.PointerPosition.Position;
			pointerVisible = frameInfo.PointerPosition.Visible != FALSE;
		}
		if (frameInfo.PointerShapeBufferSize > 0) {
			pointerShape.resize(frameInfo.PointerShapeBufferSize);
			UINT bufferSize = 0;
			const HRESULT shapeResult = duplication->GetFramePointerShape(
				frameInfo.PointerShapeBufferSize, pointerShape.data(), &bufferSize,
				&pointerShapeInfo);
			if (SUCCEEDED(shapeResult)) {
				pointerShape.resize(bufferSize);
			} else {
				pointerShape.clear();
				if (!warnedAboutCursor) {
					obs_log(LOG_WARNING,
						"Could not read the DXGI desktop pointer shape "
						"(0x%08lX).",
						static_cast<unsigned long>(shapeResult));
					warnedAboutCursor = true;
				}
			}
		}

		ComPtr<ID3D11Texture2D> desktopTexture;
		HRESULT result = resource.As(&desktopTexture);
		if (FAILED(result)) {
			obs_log(LOG_WARNING,
				"Could not access a DXGI Desktop Duplication frame texture (0x%08lX).",
				static_cast<unsigned long>(result));
			continue;
		}

		D3D11_TEXTURE2D_DESC desktopDesc{};
		desktopTexture->GetDesc(&desktopDesc);
		const LONG monitorWidth = currentMonitorRect.right - currentMonitorRect.left;
		const LONG monitorHeight = currentMonitorRect.bottom - currentMonitorRect.top;
		if (monitorWidth <= 0 || monitorHeight <= 0 || !desktopDesc.Width || !desktopDesc.Height)
			continue;

		const double scaleX = desktopDesc.Width / static_cast<double>(monitorWidth);
		const double scaleY = desktopDesc.Height / static_cast<double>(monitorHeight);
		RECT crop{
			static_cast<LONG>(std::floor((target.clientRect.left - currentMonitorRect.left) * scaleX)),
			static_cast<LONG>(std::floor((target.clientRect.top - currentMonitorRect.top) * scaleY)),
			static_cast<LONG>(std::ceil((target.clientRect.right - currentMonitorRect.left) * scaleX)),
			static_cast<LONG>(std::ceil((target.clientRect.bottom - currentMonitorRect.top) * scaleY)),
		};
		crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(desktopDesc.Width));
		crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(desktopDesc.Height));
		crop.right = std::clamp<LONG>(crop.right, 0, static_cast<LONG>(desktopDesc.Width));
		crop.bottom = std::clamp<LONG>(crop.bottom, 0, static_cast<LONG>(desktopDesc.Height));
		if (crop.right <= crop.left || crop.bottom <= crop.top)
			continue;

		const uint32_t width = static_cast<uint32_t>(crop.right - crop.left);
		const uint32_t height = static_cast<uint32_t>(crop.bottom - crop.top);
		const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4;
		if (width > kMaxCaptureDimension || height > kMaxCaptureDimension ||
		    byteCount > kMaxCaptureBytes) {
			obs_log(LOG_WARNING, "DXGI Desktop Duplication crop exceeds capture limits.");
			continue;
		}

		D3D11_TEXTURE2D_DESC stagingDesc = desktopDesc;
		stagingDesc.Width = width;
		stagingDesc.Height = height;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 1;
		stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.SampleDesc.Quality = 0;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.MiscFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		D3D11_TEXTURE2D_DESC previousDesc{};
		if (staging)
			staging->GetDesc(&previousDesc);
		if (!staging || previousDesc.Width != width || previousDesc.Height != height ||
		    previousDesc.Format != stagingDesc.Format) {
			staging.Reset();
			result = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
			if (FAILED(result)) {
				obs_log(LOG_WARNING,
					"Could not create the DXGI Desktop Duplication staging texture "
					"(0x%08lX).",
					static_cast<unsigned long>(result));
				continue;
			}
		}

		const D3D11_BOX region{static_cast<UINT>(crop.left), static_cast<UINT>(crop.top), 0,
				       static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
		context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, desktopTexture.Get(), 0,
					       &region);
		std::vector<uint8_t> framePixels(static_cast<size_t>(byteCount));
		D3D11_MAPPED_SUBRESOURCE mapped{};
		result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
		if (FAILED(result)) {
			obs_log(LOG_WARNING,
				"Could not map a DXGI Desktop Duplication frame (0x%08lX).",
				static_cast<unsigned long>(result));
			continue;
		}

		for (uint32_t y = 0; y < height; ++y)
			std::memcpy(framePixels.data() + static_cast<size_t>(y) * width * 4,
				    static_cast<const uint8_t *>(mapped.pData) +
					    static_cast<size_t>(y) * mapped.RowPitch,
				    static_cast<size_t>(width) * 4);
		context->Unmap(staging.Get(), 0);

		if (capture->captureCursor.load() && pointerVisible)
			composite_desktop_duplication_cursor(
				framePixels, width, height, width * 4, pointerShape, pointerShapeInfo,
				pointerPosition, currentMonitorRect, crop, desktopDesc.Width,
				desktopDesc.Height);

		WindowInfo stillActive{};
		if (get_window_info(target.window, processId, stillActive) &&
		    stillActive.window == target.window && stillActive.monitor == target.monitor &&
		    stillActive.clientRect.left == target.clientRect.left &&
		    stillActive.clientRect.top == target.clientRect.top &&
		    stillActive.clientRect.right == target.clientRect.right &&
		    stillActive.clientRect.bottom == target.clientRect.bottom) {
			uint64_t submissionTimeNs = 0;
			output_captured_frame(capture, framePixels.data(), width, height, width * 4,
					      os_gettime_ns(), submissionTimeNs);
		}
	}

	duplication.Reset();
	staging.Reset();
	context.Reset();
	device.Reset();
	return false;
}

void video_capture_loop(GameOutputCapture *capture)
{
	try {
		const CaptureBackend videoBackend = capture->videoBackend.load();
		video_capture_loop_impl(capture,
					videoBackend == CaptureBackend::DesktopDuplication);
	} catch (const std::bad_alloc &) {
		obs_log(LOG_ERROR, "Video capture stopped because the frame buffer could not be allocated.");
	} catch (const winrt::hresult_error &error) {
		obs_log(LOG_ERROR, "Video capture stopped unexpectedly (0x%08lX).",
			static_cast<unsigned long>(error.code()));
		output_transparent_frame(capture, kFallbackWidth, kFallbackHeight);
	}
}

class AudioActivationHandler final
	: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
					      Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
	AudioActivationHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

	~AudioActivationHandler()
	{
		if (event_)
			CloseHandle(event_);
	}

	HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override
	{
		ComPtr<IUnknown> activated;
		HRESULT activationStatus = E_FAIL;
		const HRESULT result = operation->GetActivateResult(&activationStatus, &activated);
		if (SUCCEEDED(result) && SUCCEEDED(activationStatus))
			activationStatus = activated.As(&client_);
		status_ = FAILED(result) ? result : activationStatus;
		SetEvent(event_);
		return S_OK;
	}

	HANDLE event() const { return event_; }
	HRESULT status() const { return status_.load(); }
	IAudioClient *client() const { return client_.Get(); }

private:
	HANDLE event_ = nullptr;
	std::atomic<HRESULT> status_{E_PENDING};
	ComPtr<IAudioClient> client_;
};

bool capture_audio(GameOutputCapture *capture, DWORD processId, HANDLE stopEvent)
{
	const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(comResult)) {
		obs_log(LOG_ERROR, "Could not initialize COM for process-audio capture (0x%08lX).", comResult);
		return false;
	}

	AUDIOCLIENT_ACTIVATION_PARAMS activation{};
	activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	activation.ProcessLoopbackParams.TargetProcessId = processId;
	activation.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	PROPVARIANT parameters{};
	parameters.vt = VT_BLOB;
	parameters.blob.cbSize = sizeof(activation);
	parameters.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);

	ComPtr<AudioActivationHandler> handler = Microsoft::WRL::Make<AudioActivationHandler>();
	if (!handler->event()) {
		CoUninitialize();
		obs_log(LOG_ERROR, "Could not create the process-audio activation event.");
		return false;
	}

	ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
	HRESULT result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
						     &parameters, handler.Get(), &operation);
	if (FAILED(result)) {
		CoUninitialize();
		obs_log(LOG_ERROR, "Could not request Windows process-loopback audio for PID %lu (0x%08lX).", processId,
			result);
		return false;
	}

	const DWORD waitResult = WaitForSingleObject(handler->event(), 5000);
	if (waitResult != WAIT_OBJECT_0 || FAILED(handler->status())) {
		result = waitResult == WAIT_OBJECT_0 ? handler->status() : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		CoUninitialize();
		obs_log(LOG_ERROR, "Windows process-loopback activation failed for PID %lu (0x%08lX).", processId,
			result);
		return false;
	}

	ComPtr<IAudioClient> client = handler->client();
	if (!client) {
		CoUninitialize();
		obs_log(LOG_ERROR, "Windows process-loopback activation returned no audio client.");
		return false;
	}

	obs_audio_info audioInfo{};
	if (!obs_get_audio_info(&audioInfo) || audioInfo.samples_per_sec == 0) {
		CoUninitialize();
		obs_log(LOG_ERROR, "Could not read the OBS audio output format.");
		return false;
	}
	WAVEFORMATEXTENSIBLE audioFormat{};
	audioFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	audioFormat.Format.nChannels = 2;
	audioFormat.Format.nSamplesPerSec = audioInfo.samples_per_sec;
	audioFormat.Format.wBitsPerSample = 32;
	audioFormat.Format.nBlockAlign = 2 * sizeof(float);
	audioFormat.Format.nAvgBytesPerSec = audioFormat.Format.nSamplesPerSec * audioFormat.Format.nBlockAlign;
	audioFormat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	audioFormat.Samples.wValidBitsPerSample = 32;
	audioFormat.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
	audioFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

	HANDLE packetEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!packetEvent) {
		CoUninitialize();
		obs_log(LOG_ERROR, "Could not create the process-audio packet event.");
		return false;
	}

	result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 5 * 10000000, 0,
				    &audioFormat.Format, nullptr);
	if (SUCCEEDED(result))
		result = client->SetEventHandle(packetEvent);

	ComPtr<IAudioCaptureClient> audioCapture;
	if (SUCCEEDED(result))
		result = client->GetService(IID_PPV_ARGS(&audioCapture));
	if (SUCCEEDED(result))
		result = client->Start();
	if (FAILED(result)) {
		CloseHandle(packetEvent);
		CoUninitialize();
		obs_log(LOG_ERROR, "Could not start process-loopback audio capture (0x%08lX).", result);
		return false;
	}
	obs_log(LOG_INFO, "Windows process-loopback audio started for PID %lu.", processId);

	const uint32_t sampleRate = audioFormat.Format.nSamplesPerSec;
	const uint32_t bytesPerFrame = audioFormat.Format.nBlockAlign;
	std::vector<float> left;
	std::vector<float> right;
	HANDLE events[2]{packetEvent, stopEvent};

	while (true) {
		const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (waitResult == WAIT_OBJECT_0 + 1)
			break;
		if (waitResult != WAIT_OBJECT_0) {
			result = HRESULT_FROM_WIN32(waitResult == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA);
			break;
		}

		UINT32 packetFrames = 0;
		result = audioCapture->GetNextPacketSize(&packetFrames);
		while (SUCCEEDED(result) && packetFrames > 0) {
			BYTE *buffer = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			UINT64 devicePosition = 0;
			UINT64 qpcPosition = 0;
			result = audioCapture->GetBuffer(&buffer, &frames, &flags, &devicePosition, &qpcPosition);
			if (FAILED(result))
				break;

			if (frames <= 8192) {
				left.assign(frames, 0.0f);
				right.assign(frames, 0.0f);
				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
					for (UINT32 frame = 0; frame < frames; ++frame) {
						const uint8_t *samples =
							buffer + static_cast<size_t>(frame) * bytesPerFrame;
						std::memcpy(&left[frame], samples, sizeof(float));
						std::memcpy(&right[frame], samples + sizeof(float), sizeof(float));
					}
				}

				obs_source_audio audio{};
				audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
				audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
				audio.frames = frames;
				audio.speakers = SPEAKERS_STEREO;
				audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
				audio.samples_per_sec = sampleRate;
				audio.timestamp = qpcPosition ? qpcPosition * 100 : os_gettime_ns();
				obs_source_output_audio(capture->source, &audio);
			}

			result = audioCapture->ReleaseBuffer(frames);
			if (FAILED(result))
				break;
			result = audioCapture->GetNextPacketSize(&packetFrames);
		}

		if (FAILED(result))
			break;
	}

	client->Stop();
	CloseHandle(packetEvent);
	CoUninitialize();
	if (FAILED(result)) {
		obs_log(LOG_ERROR, "Process-loopback audio stopped after a Windows audio error (0x%08lX).", result);
		return false;
	}
	return true;
}

void audio_capture_loop(GameOutputCapture *capture, DWORD processId, HANDLE stopEvent)
{
	if (!processId)
		return;

	try {
		capture_audio(capture, processId, stopEvent);
	} catch (const std::bad_alloc &) {
		obs_log(LOG_ERROR, "Process-audio capture stopped because an audio buffer could not be allocated.");
	}
}

void stop_audio_capture(GameOutputCapture *capture)
{
	std::lock_guard<std::mutex> lock(capture->audioMutex);
	if (capture->audioStopEvent)
		SetEvent(capture->audioStopEvent);
	if (capture->audioThread.joinable())
		capture->audioThread.join();
	if (capture->audioStopEvent) {
		CloseHandle(capture->audioStopEvent);
		capture->audioStopEvent = nullptr;
	}
}

void start_audio_capture(GameOutputCapture *capture, DWORD processId)
{
	std::lock_guard<std::mutex> controlLock(capture->audioControlMutex);
	stop_audio_capture(capture);
	if (!processId || capture->shuttingDown.load())
		return;

	std::lock_guard<std::mutex> lock(capture->audioMutex);
	capture->audioStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!capture->audioStopEvent) {
		obs_log(LOG_ERROR, "Could not create the process-audio shutdown event.");
		return;
	}
	HANDLE stopEvent = capture->audioStopEvent;
	try {
		capture->audioThread = std::thread(audio_capture_loop, capture, processId, stopEvent);
	} catch (const std::system_error &error) {
		CloseHandle(capture->audioStopEvent);
		capture->audioStopEvent = nullptr;
		obs_log(LOG_ERROR, "Could not start process-audio capture: %s.", error.what());
	}
}

void process_monitor_loop(GameOutputCapture *capture, HANDLE stopEvent)
{
	while (WaitForSingleObject(stopEvent, 1000) == WAIT_TIMEOUT) {
		std::string windowMatch;
		{
			std::lock_guard<std::mutex> lock(capture->targetMutex);
			windowMatch = capture->windowMatch;
		}

		const DWORD processId =
			find_process_by_window_match(windowMatch, capture->windowPriority.load());
		update_target_state(capture, windowMatch, capture->specificWindowMode.load());

		const DWORD previousProcessId = capture->processId.exchange(processId);
		if (previousProcessId != processId) {
			obs_log(LOG_INFO, "Matched application process changed from %lu to %lu.", previousProcessId,
				processId);
			start_audio_capture(capture, processId);
		}
	}
}

void start_process_monitor(GameOutputCapture *capture)
{
	if (capture->processMonitorThread.joinable() || capture->shuttingDown.load())
		return;

	capture->processMonitorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!capture->processMonitorStopEvent) {
		obs_log(LOG_ERROR, "Could not create the application monitor shutdown event.");
		return;
	}

	try {
		capture->processMonitorThread =
			std::thread(process_monitor_loop, capture, capture->processMonitorStopEvent);
	} catch (const std::system_error &error) {
		CloseHandle(capture->processMonitorStopEvent);
		capture->processMonitorStopEvent = nullptr;
		obs_log(LOG_ERROR, "Could not start the application monitor: %s.", error.what());
	}
}

void stop_process_monitor(GameOutputCapture *capture)
{
	if (capture->processMonitorStopEvent)
		SetEvent(capture->processMonitorStopEvent);
	if (capture->processMonitorThread.joinable())
		capture->processMonitorThread.join();
	if (capture->processMonitorStopEvent) {
		CloseHandle(capture->processMonitorStopEvent);
		capture->processMonitorStopEvent = nullptr;
	}
}

const char *source_name(void *)
{
	return obs_module_text("Game Output Capture");
}

const char *window_display_frame_source_name(void *)
{
	return obs_module_text("Window and Display capture frame");
}

void *window_display_frame_source_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

void window_display_frame_source_destroy(void *)
{
}

bool start_video_capture_locked(GameOutputCapture *capture, CaptureBackend backend)
{
	if (capture->videoThread.joinable() || capture->shuttingDown.load())
		return true;

	capture->videoBackend.store(backend);
	capture->videoStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!capture->videoStopEvent) {
		obs_log(LOG_ERROR, "Could not create the video capture shutdown event.");
		return false;
	}

	try {
		capture->videoThread = std::thread(video_capture_loop, capture);
	} catch (const std::system_error &error) {
		CloseHandle(capture->videoStopEvent);
		capture->videoStopEvent = nullptr;
		obs_log(LOG_ERROR, "Could not start video capture: %s.", error.what());
		return false;
	}

	obs_log(LOG_INFO, "%s capture backend enabled.", backend_display_name(backend));
	return true;
}

void stop_video_capture_locked(GameOutputCapture *capture)
{
	if (capture->videoStopEvent)
		SetEvent(capture->videoStopEvent);
	if (capture->videoThread.joinable())
		capture->videoThread.join();
	if (capture->videoStopEvent) {
		CloseHandle(capture->videoStopEvent);
		capture->videoStopEvent = nullptr;
	}
	if (capture->windowDisplaySource)
		obs_source_output_video(capture->windowDisplaySource, nullptr);
}

bool set_capture_backend(GameOutputCapture *capture, CaptureBackend nextBackend)
{
	if (!capture || capture->shuttingDown.load())
		return false;

	std::lock_guard<std::mutex> lock(capture->backendMutex);
	const CaptureBackend previousBackend = capture->backend.load();
	if (previousBackend == nextBackend)
		return true;

	const bool previousUsesVideo = previousBackend != CaptureBackend::GameCapture;
	const bool nextUsesVideo = nextBackend != CaptureBackend::GameCapture;
	if (capture->sourceShowing.load() && nextUsesVideo && !previousUsesVideo) {
		if (capture->windowDisplaySource)
			obs_source_inc_showing(capture->windowDisplaySource);
		if (!start_video_capture_locked(capture, nextBackend)) {
			if (capture->windowDisplaySource)
				obs_source_dec_showing(capture->windowDisplaySource);
			return false;
		}
	} else if (capture->sourceShowing.load() && previousUsesVideo && nextUsesVideo) {
		stop_video_capture_locked(capture);
		if (!start_video_capture_locked(capture, nextBackend)) {
			if (!start_video_capture_locked(capture, previousBackend))
				obs_log(LOG_ERROR, "Could not restore the previous video capture method.");
			return false;
		}
	}

	if (capture->sourceShowing.load() && previousBackend == CaptureBackend::GameCapture) {
		if (capture->gameCaptureSource && capture->gameCaptureShowing.exchange(false))
			obs_source_dec_showing(capture->gameCaptureSource);
	} else if (capture->sourceShowing.load() && previousUsesVideo && !nextUsesVideo) {
		stop_video_capture_locked(capture);
		if (capture->windowDisplaySource)
			obs_source_dec_showing(capture->windowDisplaySource);
	}

	capture->backend.store(nextBackend);
	if (capture->sourceShowing.load() && nextBackend == CaptureBackend::GameCapture &&
	    capture->gameCaptureSource && !capture->gameCaptureShowing.exchange(true))
		obs_source_inc_showing(capture->gameCaptureSource);

	obs_log(LOG_INFO, "Capture backend switched to %s.", backend_display_name(nextBackend));
	return true;
}

void store_capture_backend_setting(GameOutputCapture *capture, CaptureBackend backend)
{
	if (!capture->source)
		return;

	obs_data_t *settings = obs_source_get_settings(capture->source);
	obs_data_set_string(settings, SETTING_CAPTURE_BACKEND, backend_setting_value(backend));
	obs_source_update(capture->source, settings);
	obs_data_release(settings);
}

void toggle_capture_backend(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;

	auto *capture = static_cast<GameOutputCapture *>(data);
	if (!capture || capture->shuttingDown.load())
		return;

	const CaptureBackend currentBackend = capture->backend.load();
	const CaptureBackend nextBackend =
		currentBackend == CaptureBackend::GameCapture ?
			CaptureBackend::WindowDisplay :
		currentBackend == CaptureBackend::WindowDisplay ?
			CaptureBackend::DesktopDuplication :
			CaptureBackend::GameCapture;
	if (!set_capture_backend(capture, nextBackend)) {
		obs_log(LOG_ERROR, "Capture backend switch failed; keeping the current backend.");
		return;
	}
	store_capture_backend_setting(capture, nextBackend);
}

uint32_t source_width(void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	if (capture->backend.load() != CaptureBackend::GameCapture) {
		const uint32_t width =
			capture->windowDisplaySource ? obs_source_get_width(capture->windowDisplaySource) : 0;
		return width ? width : (capture->targetRunning.load() ? capture->targetWidth.load() : 0);
	}

	if (capture->gameCaptureSource) {
		const uint32_t width = obs_source_get_width(capture->gameCaptureSource);
		if (width > 0) {
			capture->targetWidth.store(width);
			return width;
		}
	}
	return capture->specificWindowMode.load() && capture->targetRunning.load() ?
		       capture->targetWidth.load() :
		       0;
}

uint32_t source_height(void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	if (capture->backend.load() != CaptureBackend::GameCapture) {
		const uint32_t height =
			capture->windowDisplaySource ? obs_source_get_height(capture->windowDisplaySource) : 0;
		return height ? height : (capture->targetRunning.load() ? capture->targetHeight.load() : 0);
	}

	if (capture->gameCaptureSource) {
		const uint32_t height = obs_source_get_height(capture->gameCaptureSource);
		if (height > 0) {
			capture->targetHeight.store(height);
			return height;
		}
	}
	return capture->specificWindowMode.load() && capture->targetRunning.load() ?
		       capture->targetHeight.load() :
		       0;
}

void source_render(void *data, gs_effect_t *)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	if (capture->backend.load() != CaptureBackend::GameCapture) {
		if (capture->windowDisplaySource)
			obs_source_video_render(capture->windowDisplaySource);
		return;
	}

	if (capture->gameCaptureSource && obs_source_get_width(capture->gameCaptureSource) > 0 &&
	    obs_source_get_height(capture->gameCaptureSource) > 0) {
		obs_source_video_render(capture->gameCaptureSource);
		return;
	}

	if (!capture->specificWindowMode.load() || !capture->targetRunning.load())
		return;

	const uint32_t width = capture->targetWidth.load();
	const uint32_t height = capture->targetHeight.load();
	if (!width || !height)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 black{};
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &black);
	gs_technique_t *technique = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(technique);
	gs_technique_begin_pass(technique, 0);
	gs_draw_sprite(nullptr, 0, width, height);
	gs_technique_end_pass(technique);
	gs_technique_end(technique);
}

void source_show(void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	std::lock_guard<std::mutex> lock(capture->backendMutex);
	capture->sourceShowing.store(true);
	if (capture->backend.load() == CaptureBackend::GameCapture) {
		if (capture->gameCaptureSource && !capture->gameCaptureShowing.exchange(true))
			obs_source_inc_showing(capture->gameCaptureSource);
	} else {
		if (capture->windowDisplaySource)
			obs_source_inc_showing(capture->windowDisplaySource);
		if (!start_video_capture_locked(capture, capture->backend.load())) {
			if (capture->windowDisplaySource)
				obs_source_dec_showing(capture->windowDisplaySource);
			capture->backend.store(CaptureBackend::GameCapture);
			if (capture->gameCaptureSource && !capture->gameCaptureShowing.exchange(true))
				obs_source_inc_showing(capture->gameCaptureSource);
		}
	}
}

void source_hide(void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	std::lock_guard<std::mutex> lock(capture->backendMutex);
	capture->sourceShowing.store(false);
	if (capture->backend.load() == CaptureBackend::GameCapture) {
		if (capture->gameCaptureSource && capture->gameCaptureShowing.exchange(false))
			obs_source_dec_showing(capture->gameCaptureSource);
	} else {
		stop_video_capture_locked(capture);
		if (capture->windowDisplaySource)
			obs_source_dec_showing(capture->windowDisplaySource);
	}
}

void source_enum_active_sources(void *data, obs_source_enum_proc_t callback, void *parameter)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	if (capture->backend.load() == CaptureBackend::GameCapture && capture->gameCaptureSource)
		callback(capture->source, capture->gameCaptureSource, parameter);
	else if (capture->windowDisplaySource)
		callback(capture->source, capture->windowDisplaySource, parameter);
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_CAPTURE_BACKEND, kBackendGameCapture);
	obs_data_set_default_string(settings, SETTING_MODE, kModeWindow);
	obs_data_set_default_string(settings, SETTING_WINDOW, "");
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY, WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_CURSOR, false);
}

void populate_window_list(obs_property_t *property, obs_data_t *settings)
{
	obs_property_list_clear(property);
	obs_property_list_add_string(property, obs_module_text("Select a window to capture"), "");
	ms_fill_window_list(property, INCLUDE_MINIMIZED, nullptr);
	if (settings)
		ms_check_window_property_setting(nullptr, property, settings, SETTING_WINDOW, 1);
}

bool mode_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const bool captureWindow = strcmp(obs_data_get_string(settings, SETTING_MODE), kModeWindow) == 0;
	obs_property_set_visible(obs_properties_get(properties, SETTING_WINDOW), captureWindow);
	obs_property_set_visible(obs_properties_get(properties, SETTING_WINDOW_PRIORITY), captureWindow);
	return true;
}

bool window_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	return ms_check_window_property_setting(properties, property, settings, SETTING_WINDOW, 1);
}

bool refresh_window_list(obs_properties_t *properties, obs_property_t *, void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	obs_property_t *property = obs_properties_get(properties, SETTING_WINDOW);
	if (!property)
		return true;

	obs_data_t *settings = capture && capture->source ? obs_source_get_settings(capture->source) : nullptr;
	populate_window_list(property, settings);
	if (settings)
		obs_data_release(settings);
	return true;
}

obs_properties_t *source_properties(void *data)
{
	obs_properties_t *properties = obs_properties_create();
	auto *capture = static_cast<GameOutputCapture *>(data);

	obs_property_t *backend =
		obs_properties_add_list(properties, SETTING_CAPTURE_BACKEND,
					"Capture method", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(backend, "Game Capture", kBackendGameCapture);
	obs_property_list_add_string(backend, "Window and Display", kBackendWindowDisplay);
	obs_property_list_add_string(backend, obs_module_text("Window and DXGI"), kBackendWindowDXGI);

	obs_property_t *mode = obs_properties_add_list(properties, SETTING_MODE, obs_module_text("Capture mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("Capture any fullscreen application"), kModeAny);
	obs_property_list_add_string(mode, obs_module_text("Capture specific window"), kModeWindow);
	obs_property_set_modified_callback(mode, mode_modified);

	obs_data_t *settings = capture && capture->source ? obs_source_get_settings(capture->source) : nullptr;
	obs_property_t *window = obs_properties_add_list(properties, SETTING_WINDOW, obs_module_text("Window"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_window_list(window, settings);
	obs_property_set_modified_callback(window, window_modified);

	obs_property_t *priority =
		obs_properties_add_list(properties, SETTING_WINDOW_PRIORITY, obs_module_text("Window Match Priority"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(priority, obs_module_text("Window title must match"), WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(priority, obs_module_text("Match title, otherwise find window of same type"),
				  WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(priority,
				  obs_module_text("Match title, otherwise find window of same executable"),
				  WINDOW_PRIORITY_EXE);

	obs_properties_add_button2(properties, "refresh_windows", obs_module_text("Refresh windows"),
				   refresh_window_list, capture);
	obs_properties_add_bool(properties, SETTING_CURSOR, obs_module_text("Capture cursor"));
	obs_properties_add_text(properties, "capture_behavior_notice",
				obs_module_text("Capture behavior notice"), OBS_TEXT_INFO);
	if (settings) {
		mode_modified(properties, nullptr, settings);
		obs_data_release(settings);
	}
	return properties;
}

void update_source(void *data, obs_data_t *settings)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	const char *backendSetting = obs_data_get_string(settings, SETTING_CAPTURE_BACKEND);
	const CaptureBackend requestedBackend = backend_from_setting(backendSetting);
	if (!set_capture_backend(capture, requestedBackend))
		obs_log(LOG_ERROR, "Could not switch to the selected capture method.");

	const char *mode = obs_data_get_string(settings, SETTING_MODE);
	const std::string windowMatch = migrate_legacy_window_match(settings);
	const int priority = static_cast<int>(obs_data_get_int(settings, SETTING_WINDOW_PRIORITY));
	const bool captureCursor = obs_data_get_bool(settings, SETTING_CURSOR);
	capture->captureCursor.store(captureCursor);
	update_target_state(capture, windowMatch, strcmp(mode, kModeWindow) == 0);
	obs_data_t *gameCaptureSettings =
		create_game_capture_settings(mode, windowMatch.c_str(), priority, captureCursor);
	if (!gameCaptureSettings) {
		obs_log(LOG_ERROR, "Could not create settings for OBS Game Capture.");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(capture->targetMutex);
		capture->windowMatch = windowMatch;
	}
	capture->windowPriority.store(priority);
	if (capture->gameCaptureSource)
		obs_source_update(capture->gameCaptureSource, gameCaptureSettings);
	obs_data_release(gameCaptureSettings);

	const DWORD processId = find_process_by_window_match(windowMatch, priority);
	const DWORD previousProcessId = capture->processId.exchange(processId);
	if (previousProcessId != processId) {
		obs_log(LOG_INFO, "Matched application process id: %lu.", processId);
		start_audio_capture(capture, processId);
	}
	start_process_monitor(capture);
}

void *create_source(obs_data_t *settings, obs_source_t *source)
{
	GameOutputCapture *capture = nullptr;
	try {
		capture = new GameOutputCapture();
	} catch (const std::bad_alloc &) {
		obs_log(LOG_ERROR, "Could not allocate the game output capture source.");
		return nullptr;
	}
	capture->source = source;
	const char *backendSetting = obs_data_get_string(settings, SETTING_CAPTURE_BACKEND);
	capture->backend.store(backend_from_setting(backendSetting));
	const char *mode = obs_data_get_string(settings, SETTING_MODE);
	const std::string windowMatch = migrate_legacy_window_match(settings);
	const int priority = static_cast<int>(obs_data_get_int(settings, SETTING_WINDOW_PRIORITY));
	capture->windowPriority.store(priority);
	capture->captureCursor.store(obs_data_get_bool(settings, SETTING_CURSOR));
	update_target_state(capture, windowMatch, strcmp(mode, kModeWindow) == 0);
	{
		std::lock_guard<std::mutex> lock(capture->targetMutex);
		capture->windowMatch = windowMatch;
	}
	const DWORD processId = find_process_by_window_match(windowMatch, priority);
	capture->processId.store(processId);

	obs_data_t *gameCaptureSettings =
		create_game_capture_settings(mode, windowMatch.c_str(), priority, capture->captureCursor.load());
	if (!gameCaptureSettings) {
		obs_log(LOG_ERROR, "Could not create settings for OBS Game Capture.");
		delete capture;
		return nullptr;
	}
	capture->gameCaptureSource = obs_source_create_private(
		"game_capture", "Game Output Capture internal hook", gameCaptureSettings);
	obs_data_release(gameCaptureSettings);
	if (!capture->gameCaptureSource) {
		obs_log(LOG_ERROR,
			"Could not create the internal OBS Game Capture source. Ensure OBS's win-capture "
			"module is loaded.");
		delete capture;
		return nullptr;
	}
	if (processId)
		start_audio_capture(capture, processId);
	obs_data_t *frameSourceSettings = obs_data_create();
	capture->windowDisplaySource = obs_source_create_private(kWindowDisplayFrameSourceId,
								 "Window/Display capture frames",
								 frameSourceSettings);
	obs_data_release(frameSourceSettings);
	if (!capture->windowDisplaySource) {
		obs_log(LOG_ERROR, "Could not create the internal Window/Display frame source.");
		obs_source_release(capture->gameCaptureSource);
		capture->gameCaptureSource = nullptr;
		stop_audio_capture(capture);
		delete capture;
		return nullptr;
	}
	capture->backendToggleHotkey = obs_hotkey_register_source(
		source, kBackendToggleHotkeyName, obs_module_text("Cycle capture method"),
		toggle_capture_backend, capture);
	start_process_monitor(capture);
	return capture;
}

void destroy_source(void *data)
{
	auto *capture = static_cast<GameOutputCapture *>(data);
	const CaptureBackend currentBackend = capture->backend.load();
	obs_log(LOG_INFO, "Destroying capture source (backend=%s, video_thread=%s).",
		backend_display_name(currentBackend),
		capture->videoThread.joinable() ? "running" : "stopped");
	capture->shuttingDown.store(true);
	stop_process_monitor(capture);
	stop_audio_capture(capture);
	{
		std::lock_guard<std::mutex> lock(capture->backendMutex);
		if (capture->backendToggleHotkey != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(capture->backendToggleHotkey);
			capture->backendToggleHotkey = OBS_INVALID_HOTKEY_ID;
		}
		stop_video_capture_locked(capture);
		if (capture->gameCaptureSource && capture->gameCaptureShowing.exchange(false))
			obs_source_dec_showing(capture->gameCaptureSource);
		if (capture->windowDisplaySource && capture->sourceShowing.exchange(false) &&
		    capture->backend.load() != CaptureBackend::GameCapture)
			obs_source_dec_showing(capture->windowDisplaySource);
	}
	if (capture->windowDisplaySource)
		obs_source_release(capture->windowDisplaySource);
	if (capture->gameCaptureSource) {
		obs_source_release(capture->gameCaptureSource);
	}
	obs_log(LOG_INFO, "Capture source teardown complete.");
	delete capture;
}

void register_game_output_capture_source()
{
	static obs_source_info windowDisplayFrameSourceInfo{};
	windowDisplayFrameSourceInfo.id = kWindowDisplayFrameSourceId;
	windowDisplayFrameSourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	windowDisplayFrameSourceInfo.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_SRGB;
	windowDisplayFrameSourceInfo.get_name = window_display_frame_source_name;
	windowDisplayFrameSourceInfo.create = window_display_frame_source_create;
	windowDisplayFrameSourceInfo.destroy = window_display_frame_source_destroy;
	obs_register_source(&windowDisplayFrameSourceInfo);

	static obs_source_info sourceInfo{};
	sourceInfo.id = "game_output_capture";
	sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	sourceInfo.output_flags =
		OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_SELF_MONITOR |
		OBS_SOURCE_SRGB;
	sourceInfo.get_name = source_name;
	sourceInfo.create = create_source;
	sourceInfo.destroy = destroy_source;
	sourceInfo.get_width = source_width;
	sourceInfo.get_height = source_height;
	sourceInfo.get_defaults = source_defaults;
	sourceInfo.get_properties = source_properties;
	sourceInfo.update = update_source;
	sourceInfo.show = source_show;
	sourceInfo.hide = source_hide;
	sourceInfo.enum_active_sources = source_enum_active_sources;
	sourceInfo.video_render = source_render;
	sourceInfo.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
	obs_register_source(&sourceInfo);
}

} // namespace

extern "C" void game_output_capture_register()
{
	register_game_output_capture_source();
}
