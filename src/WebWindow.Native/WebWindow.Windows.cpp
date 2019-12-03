#include "WebWindow.h"
#include <stdio.h>
#include <map>
#include <mutex>
#include <condition_variable>
#include <comdef.h>
#include <atomic>
#include <Shlwapi.h>

#define WM_USER_SHOWMESSAGE (WM_USER + 0x0001)
#define WM_USER_INVOKE (WM_USER + 0x0002)

using namespace Microsoft::WRL;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
std::wstring Utf8ToLPWSTR(UTF8String str);
LPCWSTR CLASS_NAME = L"WebWindow";
std::mutex invokeLockMutex;
HINSTANCE WebWindow::_hInstance;
HWND messageLoopRootWindowHandle;
std::map<HWND, WebWindow*> hwndToWebWindow;

struct InvokeWaitInfo
{
	std::condition_variable completionNotifier;
	bool isCompleted;
};

struct ShowMessageParams
{
	std::wstring title;
	std::wstring body;
	UINT type;
};

void WebWindow::Register(HINSTANCE hInstance)
{
	_hInstance = hInstance;

	// Register the window class	
	WNDCLASSW wc = { };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;
	RegisterClassW(&wc);

	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

WebWindow::WebWindow(UTF8String title, WebWindow* parent, WebMessageReceivedCallback webMessageReceivedCallback)
{
	// Create the window
	_webMessageReceivedCallback = webMessageReceivedCallback;
	_parent = parent;
	std::wstring wtitle = Utf8ToLPWSTR(title);
	_hWnd = CreateWindowExW(
		0,                              // Optional window styles.
		CLASS_NAME,                     // Window class
		wtitle.c_str(),					// Window text
		WS_OVERLAPPEDWINDOW,            // Window style

		// Size and position
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,

		parent ? parent->_hWnd : NULL,       // Parent window
		NULL,       // Menu
		_hInstance, // Instance handle
		this        // Additional application data
	);
	hwndToWebWindow[_hWnd] = this;
}

HWND WebWindow::getHwnd()
{
	return _hWnd;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_DESTROY:
	{
		// Only terminate the message loop if the window being closed is the one that
		// started the message loop
		hwndToWebWindow.erase(hwnd);
		if (hwnd == messageLoopRootWindowHandle)
		{
			PostQuitMessage(0);
		}
		return 0;
	}
	case WM_USER_SHOWMESSAGE:
	{
		ShowMessageParams* params = (ShowMessageParams*)wParam;
		MessageBoxW(hwnd, params->body.c_str(), params->title.c_str(), params->type);
		delete params;
		return 0;
	}

	case WM_USER_INVOKE:
	{
		ACTION callback = (ACTION)wParam;
		callback();
		InvokeWaitInfo* waitInfo = (InvokeWaitInfo*)lParam;
		{
			std::lock_guard<std::mutex> guard(invokeLockMutex);
			waitInfo->isCompleted = true;
		}
		waitInfo->completionNotifier.notify_one();
		return 0;
	}
	case WM_SIZE:
	{
		WebWindow* webWindow = hwndToWebWindow[hwnd];
		if (webWindow)
		{
			webWindow->RefitContent();
		}
		return 0;
	}
	break;
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void WebWindow::RefitContent()
{
	if (_webviewWindow)
	{
		RECT bounds;
		GetClientRect(_hWnd, &bounds);
		_webviewWindow->put_Bounds(bounds);
	}
}

void WebWindow::SetTitle(UTF8String title)
{
	std::wstring wtitle = Utf8ToLPWSTR(title);
	SetWindowTextW(_hWnd, wtitle.c_str());
}

void WebWindow::Show()
{
	ShowWindow(_hWnd, SW_SHOWDEFAULT);

	// Strangely, it only works to create the webview2 *after* the window has been shown,
	// so defer it until here. This unfortunately means you can't call the Navigate methods
	// until the window is shown.
	if (!_webviewWindow)
	{
		AttachWebView();
	}
}

void WebWindow::WaitForExit()
{
	messageLoopRootWindowHandle = _hWnd;

	// Run the message loop
	MSG msg = { };
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

void WebWindow::ShowMessage(UTF8String title, UTF8String body, UINT type)
{
	ShowMessageParams* params = new ShowMessageParams;
	params->title = Utf8ToLPWSTR(title);
	params->body = Utf8ToLPWSTR(body);
	params->type = type;
	PostMessageW(_hWnd, WM_USER_SHOWMESSAGE, (WPARAM)params, 0);
}

void WebWindow::Invoke(ACTION callback)
{
	InvokeWaitInfo waitInfo = {};
	PostMessageW(_hWnd, WM_USER_INVOKE, (WPARAM)callback, (LPARAM)&waitInfo);

	// Block until the callback is actually executed and completed
	// TODO: Add return values, exception handling, etc.
	std::unique_lock<std::mutex> uLock(invokeLockMutex);
	waitInfo.completionNotifier.wait(uLock, [&] { return waitInfo.isCompleted; });
}

std::wstring Utf8ToLPWSTR(UTF8String str)
{
	int wchars_num = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	std::wstring wstr(wchars_num, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr.data(), wchars_num);
	return wstr;
}

std::string LPWSTRToUtf8(LPWSTR str)
{
	int utf8chars_num = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
	std::string utf8(utf8chars_num, '\0');
	WideCharToMultiByte(CP_UTF8, 0, str, -1, utf8.data(), utf8chars_num, NULL, NULL);
	return utf8;
}

void WebWindow::AttachWebView()
{
	std::atomic_flag flag = ATOMIC_FLAG_INIT;
	flag.test_and_set();

	HRESULT envResult = CreateWebView2EnvironmentWithDetails(nullptr, nullptr, nullptr,
		Callback<IWebView2CreateWebView2EnvironmentCompletedHandler>(
			[&, this](HRESULT result, IWebView2Environment* env) -> HRESULT {
				_webviewEnvironment = env;

				// Create a WebView, whose parent is the main window hWnd
				env->CreateWebView(_hWnd, Callback<IWebView2CreateWebViewCompletedHandler>(
					[&, this](HRESULT result, IWebView2WebView* webview) -> HRESULT {
						if (webview != nullptr) {
							_webviewWindow = webview;
						}

						// Add a few settings for the webview
						// this is a redundant demo step as they are the default settings values
						IWebView2Settings* Settings;
						_webviewWindow->get_Settings(&Settings);
						Settings->put_IsScriptEnabled(TRUE);
						Settings->put_AreDefaultScriptDialogsEnabled(TRUE);
						Settings->put_IsWebMessageEnabled(TRUE);

						// Register interop APIs
						EventRegistrationToken webMessageToken;
						_webviewWindow->AddScriptToExecuteOnDocumentCreated(L"window.external = { sendMessage: function(message) { window.chrome.webview.postMessage(message); }, receiveMessage: function(callback) { window.chrome.webview.addEventListener(\'message\', function(e) { callback(e.data); }); } };", nullptr);
						_webviewWindow->add_WebMessageReceived(Callback<IWebView2WebMessageReceivedEventHandler>(
							[this](IWebView2WebView* webview, IWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
								wil::unique_cotaskmem_string message;
								args->get_WebMessageAsString(&message);
								std::string messageUtf8 = LPWSTRToUtf8(message.get());
								_webMessageReceivedCallback(messageUtf8.data());
								return S_OK;
							}).Get(), &webMessageToken);

						EventRegistrationToken webResourceRequestedToken;
						_webviewWindow->add_WebResourceRequested(nullptr, nullptr, 0, Callback<IWebView2WebResourceRequestedEventHandler>(
							[this](IWebView2WebView* sender, IWebView2WebResourceRequestedEventArgs* args)
							{
								IWebView2WebResourceRequest* req;
								args->get_Request(&req);

								wil::unique_cotaskmem_string uri;
								req->get_Uri(&uri);
								std::string uriString = LPWSTRToUtf8(uri.get());
								size_t colonPos = uriString.find(':', 0);
								if (colonPos > 0)
								{
									std::string scheme = uriString.substr(0, colonPos);
									WebResourceRequestedCallback handler = _schemeToRequestHandler[scheme];
									if (handler != NULL)
									{
										int numBytes;
										UTF8String contentType;
										wil::unique_cotaskmem dotNetResponse(handler(uriString.data(), &numBytes, &contentType));

										if (dotNetResponse != nullptr && contentType != nullptr)
										{
											std::wstring contentTypeWS = Utf8ToLPWSTR(contentType);

											IStream* dataStream = SHCreateMemStream((BYTE*)dotNetResponse.get(), numBytes);
											wil::com_ptr<IWebView2WebResourceResponse> response;
											_webviewEnvironment->CreateWebResourceResponse(
												dataStream, 200, L"OK", (L"Content-Type: " + contentTypeWS).c_str(),
												&response);
											args->put_Response(response.get());
										}
									}
								}

								return S_OK;
							}
						).Get(), &webResourceRequestedToken);

						RefitContent();

						flag.clear();
						return S_OK;
					}).Get());
				return S_OK;
			}).Get());

	if (envResult != S_OK)
	{
		_com_error err(envResult);
		LPCTSTR errMsg = err.ErrorMessage();
		MessageBox(_hWnd, errMsg, L"Error instantiating webview", MB_OK);
	}
	else
	{
		// Block until it's ready. This simplifies things for the caller, so they
		// don't need to regard this process as async.
		MSG msg = { };
		while (flag.test_and_set() && GetMessage(&msg, NULL, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}

void WebWindow::NavigateToUrl(UTF8String url)
{
	std::wstring urlW = Utf8ToLPWSTR(url);
	_webviewWindow->Navigate(urlW.c_str());
}

void WebWindow::NavigateToString(UTF8String content)
{
	std::wstring contentW = Utf8ToLPWSTR(content);
	_webviewWindow->NavigateToString(contentW.c_str());
}

void WebWindow::SendMessage(UTF8String message)
{
	std::wstring messageW = Utf8ToLPWSTR(message);
	_webviewWindow->PostWebMessageAsString(messageW.c_str());
}

void WebWindow::AddCustomScheme(UTF8String scheme, WebResourceRequestedCallback requestHandler)
{
	_schemeToRequestHandler[scheme] = requestHandler;
}
