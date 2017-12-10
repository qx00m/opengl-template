
#include "shared.h"

////////
//
// Windows Platform
//

#define STRICT
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#pragma warning(push, 3)
#include <Windows.h>
#pragma warning(pop)

#define WGL_DRAW_TO_WINDOW_ARB                  0x2001
#define WGL_ACCELERATION_ARB                    0x2003
#define WGL_SUPPORT_OPENGL_ARB                  0x2010
#define WGL_DOUBLE_BUFFER_ARB                   0x2011
#define WGL_PIXEL_TYPE_ARB                      0x2013
#define WGL_COLOR_BITS_ARB                      0x2014
#define WGL_ALPHA_BITS_ARB                      0x201B
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB        0x20A9

#define WGL_FULL_ACCELERATION_ARB               0x2027
#define WGL_TYPE_RGBA_ARB                       0x202B

#define WGL_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB           0x2092
#define WGL_CONTEXT_LAYER_PLANE_ARB             0x2093
#define WGL_CONTEXT_FLAGS_ARB                   0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB            0x9126

#define WGL_CONTEXT_DEBUG_BIT_ARB               0x0001
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB        0x00000001

static wchar_t *global_codename;
static wchar_t *global_loadedname;
static wchar_t *global_lockname;
static HMODULE global_code;
static FILETIME global_lastwrite;
static void *global_userdata;

#define X(ret, name, ...)	\
	static ret (*name)(__VA_ARGS__) = 0;
	CODE_FUNCTIONS
#undef X

////////
//
// Services provided by the platform.
//

internal inline void *
sys_allocate(size_t n, size_t alignment)
{
	if (n == 0)
		return 0;

	void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
	assert((uintptr_t)p % alignment == 0);
	return p;
}

internal inline void
sys_deallocate(void *p, size_t n, size_t alignment)
{
	unused(n);
	unused(alignment);

	if (p == 0)
		return;

	HeapFree(GetProcessHeap(), 0, p);
}

struct font *
sys_create_font(const wchar_t *name, i32 pixel_height)
{
	struct font *result = sys_allocate(sizeof(struct font), _Alignof(struct font));

	HDC dc = CreateCompatibleDC(0);
	result->sys = dc;

	SetBkColor(dc, RGB(0, 0, 0));
	SetTextColor(dc, RGB(255, 255, 255));

	i32 logical_height = -MulDiv(pixel_height, GetDeviceCaps(dc, LOGPIXELSY), 72);
	HFONT hfont = CreateFont(logical_height, 0, 0, 0,
				FW_NORMAL, FALSE, FALSE, FALSE,
				ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
				name);
	SelectObject(dc, hfont);

	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	result->default_x = tm.tmMaxCharWidth;
	result->default_y = tm.tmHeight;
	result->bitmap_width = tm.tmMaxCharWidth * 3;
	result->bitmap_height = tm.tmHeight * 3;

	result->ascent = tm.tmDescent;
	result->descent = tm.tmAscent;
	result->height = tm.tmHeight;
	result->external_leading = tm.tmExternalLeading;

	BITMAPINFO bi = {
		.bmiHeader.biSize = sizeof(bi.bmiHeader),
		.bmiHeader.biWidth = result->bitmap_width,
		.bmiHeader.biHeight = result->bitmap_height,
		.bmiHeader.biPlanes = 1,
		.bmiHeader.biBitCount = 32,
		.bmiHeader.biCompression = BI_RGB
	};
	HBITMAP hbitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &result->bits, 0, 0);
	SelectObject(dc, hbitmap);

	result->glyphs_max = 256;
	result->glyphs_used = 0;
	result->glyphs = allocate(struct glyph, result->glyphs_max);

	return result;
}

i32
sys_render_glyph(struct font *font, u32 codepoint)
{
	i32 result = 0;

	ABC abc;
	GetCharABCWidths(font->sys, codepoint, codepoint, &abc);
	result = abc.abcA + abc.abcB + abc.abcC;

	wchar_t text[2];
	int n = 0;
	if (codepoint < 0x10000) {
		text[0] = (wchar_t)codepoint;
		n = 1;
	}
	else {
		text[0] = (wchar_t)(((codepoint - 0x10000) >> 10) + 0xD800);
		text[1] = (wchar_t)(((codepoint - 0x10000) & 0x3FF) + 0xDC00);
		n = 2;
	}

	u32 *dst = font->bits;
	i32 i = font->bitmap_width * font->bitmap_height;
	while (i--)
		*dst++ = 0;

	TextOut(font->sys, font->default_x, font->default_y, text, n);

	return result;
}

////////

internal inline const char *
skip_space(const char *s)
{
	while (*s == ' ')
		++s;
	return s;
}

internal inline bool
token_match(const char *b, const char *e, const char *s)
{
	while (b != e && *s && *b == *s) {
		++b;
		++s;
	}
	return (b == e) && (*s == 0);
}

internal inline const char *
token_end(const char *s)
{
	while (*s && *s != ' ')
		++s;
	return s;
}

internal inline wchar_t *
make_filename(DWORD n, const wchar_t *path, const wchar_t *name)
{
	DWORD len = 0;
	while (name[len] != 0)
		++len;

	wchar_t *result = allocate(wchar_t, len + n + 1);

	wchar_t *dst = result;
	while (n--)
		*dst++ = *path++;
	while (len--)
		*dst++ = *name++;
	*dst = 0;

	return result;
}

internal void
reload_code(void)
{
	WIN32_FILE_ATTRIBUTE_DATA fd;

	if (GetFileAttributesEx(global_codename, GetFileExInfoStandard, &fd)) {
		if (CompareFileTime(&fd.ftLastWriteTime, &global_lastwrite) == 1) {
			if (global_code) {
				FreeLibrary(global_code);
				global_code = 0;
			}
			while (GetFileAttributes(global_lockname) != INVALID_FILE_ATTRIBUTES)
				Sleep(0);
			BOOL ok = CopyFile(global_codename, global_loadedname, FALSE);
			assert(ok);

			ok = GetFileAttributesEx(global_codename, GetFileExInfoStandard, &fd);
			assert(ok);

			global_code = LoadLibraryEx(global_loadedname, 0, 0);
			assert(global_code);
			global_lastwrite = fd.ftLastWriteTime;

			#define X(ret, name, ...)	\
				name = (ret (*)(__VA_ARGS__))GetProcAddress(global_code, #name);	\
				assert(name);

				CODE_FUNCTIONS
			#undef X

			#define X(ret, name, ...)		\
				do {				\
					ret (**fn)(__VA_ARGS__) = (ret (**)(__VA_ARGS__))GetProcAddress(global_code, #name);	\
					assert(fn);		\
					*fn = (ret (*)(__VA_ARGS__))wglGetProcAddress(#name);	\
					if (!*fn) {		\
						*fn = (ret (*)(__VA_ARGS__))GetProcAddress(GetModuleHandleA("opengl32.dll"), #name);	\
						assert(*fn);	\
					}			\
				} while (0);

				OPENGL_FUNCTIONS
			#undef X

			#define X(ret, name, ...)		\
				do {				\
					ret (**fn)(__VA_ARGS__) = (ret (**)(__VA_ARGS__))GetProcAddress(global_code, #name);	\
					assert(fn);		\
					*fn = name;	\
				} while (0);

				SYSTEM_FUNCTIONS
			#undef X

			global_userdata = reload(global_userdata);
		}
	}
}

internal void
WinRender(HWND hwnd, HDC dc)
{
	RECT r;
	GetClientRect(hwnd, &r);
	i32 w = r.right - r.left;
	i32 h = r.bottom - r.top;

	render(global_userdata, w, h);

	BOOL ok = SwapBuffers(dc);
	assert(ok);
}

internal void
WinMouse(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	RECT r;
	GetClientRect(hwnd, &r);
	i32 h = r.bottom - r.top;

	i32 x = (i16)(lParam & 0xFFFF);
	i32 y = h - (i16)((lParam >> 16) & 0xFFFF) - 1;

	u32 buttons = 0;
	if (wParam & MK_LBUTTON) buttons |= BUTTON_LEFT;
	if (wParam & MK_RBUTTON) buttons |= BUTTON_RIGHT;

	mouse(global_userdata, x, y, buttons);

}

internal LRESULT CALLBACK
WindowProcMain(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = 0;

	switch (uMsg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			BeginPaint(hwnd, &ps);
			WinRender(hwnd, ps.hdc);
			EndPaint(hwnd, &ps);
		} break;

		case WM_DESTROY: {
			PostQuitMessage(0);
		} break;

		case WM_LBUTTONDOWN: {
			if ((wParam & MK_RBUTTON) != MK_RBUTTON)
				SetCapture(hwnd);
			WinMouse(hwnd, wParam, lParam);
		} break;

		case WM_LBUTTONUP: {
			if ((wParam & MK_RBUTTON) != MK_RBUTTON)
				ReleaseCapture();
			WinMouse(hwnd, wParam, lParam);
		} break;

		case WM_RBUTTONDOWN: {
			if ((wParam & MK_LBUTTON) != MK_LBUTTON)
				SetCapture(hwnd);
			WinMouse(hwnd, wParam, lParam);
		} break;

		case WM_RBUTTONUP: {
			if ((wParam & MK_LBUTTON) != MK_LBUTTON)
				ReleaseCapture();
			WinMouse(hwnd, wParam, lParam);
		} break;

		case WM_MOUSEMOVE: {
			WinMouse(hwnd, wParam, lParam);
		} break;

		case WM_CHAR: {
			// wParam
		} break;

		case WM_UNICHAR: {
			if (wParam == UNICODE_NOCHAR) {
				result = TRUE;
			}
			else {
				// wParam
			}
		} break;

		default: {
			result = DefWindowProc(hwnd, uMsg, wParam, lParam);
		} break;
	}

	return result;
}

void WinEntry(void)
{
	const char *(*wglGetExtensionsStringARB)(HDC hdc) = 0;
    	BOOL (*wglChoosePixelFormatARB)(HDC hdc, const int *piAttribIList, const FLOAT *pfAttribFList, UINT nMaxFormats, int *piFormats, UINT *nNumFormats) = 0;
    	HGLRC (*wglCreateContextAttribsARB)(HDC hDC, HGLRC hShareContext, const int *attribList) = 0;
    	BOOL (*wglSwapIntervalEXT)(int interval) = 0;
    	bool has_srgb_framebuffer = false;

    	SetProcessDPIAware();

    	////////
    	//
    	// init hot code reloading.
    	//
    	{
    		DWORD m = 16;
    		wchar_t *exename = allocate(wchar_t, m);
    		DWORD n;
    		while ((n = GetModuleFileName(0, exename, m)) == m) {
    			sys_deallocate(exename, m * sizeof(wchar_t), _Alignof(wchar_t));
    			m *= 2;
    			exename = allocate(wchar_t, m);
    		}

    		while (n && exename[n - 1] != L'\\')
    			--n;

    		global_codename = make_filename(n, exename, L"code.dll");
    		global_loadedname = make_filename(n, exename, L"loaded.dll");
    		global_lockname = make_filename(n, exename, L"build.lock");

    		sys_deallocate(exename, m * sizeof(wchar_t), _Alignof(wchar_t));
    	}

    	////////
    	//
	// load windows OpenGL extensions.
	{
		RegisterClass(&(WNDCLASS){
			.lpfnWndProc = DefWindowProc,
			.hInstance = GetModuleHandle(0),
			.lpszClassName = L"tmp"
		});

		HWND hwnd = CreateWindowEx(0, L"tmp", L"", WS_OVERLAPPEDWINDOW,
					CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
					0, 0, GetModuleHandle(0), 0);
		HDC dc = GetDC(hwnd);

		PIXELFORMATDESCRIPTOR pfd = {
			.nSize = sizeof(pfd),
			.nVersion = 1,
			.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
			.iPixelType = PFD_TYPE_RGBA,
			.cColorBits = 24
		};
		int pf = ChoosePixelFormat(dc, &pfd);
		SetPixelFormat(dc, pf, &pfd);
		HGLRC rc = wglCreateContext(dc);
		wglMakeCurrent(dc, rc);

		wglGetExtensionsStringARB = (const char *(*)(HDC))wglGetProcAddress("wglGetExtensionsStringARB");
		if (wglGetExtensionsStringARB) {
			const char *b = wglGetExtensionsStringARB(dc);
			if (b) {
				while (*b) {
					b = skip_space(b);
					const char *e = token_end(b);

					if (!wglChoosePixelFormatARB && token_match(b, e, "WGL_ARB_pixel_format"))
						wglChoosePixelFormatARB = (BOOL (*)(HDC, const int *, const FLOAT *, UINT, int *, UINT *))wglGetProcAddress("wglChoosePixelFormatARB");
					else if (!wglCreateContextAttribsARB && token_match(b, e, "WGL_ARB_create_context_profile"))
						wglCreateContextAttribsARB = (HGLRC (*)(HDC, HGLRC, const int *))wglGetProcAddress("wglCreateContextAttribsARB");
					else if (!wglSwapIntervalEXT && token_match(b, e, "WGL_EXT_swap_control"))
						wglSwapIntervalEXT = (BOOL (*)(int))wglGetProcAddress("wglSwapIntervalEXT");
					else if (!has_srgb_framebuffer && token_match(b, e, "WGL_EXT_framebuffer_sRGB"))
						has_srgb_framebuffer = true;

					b = e;
				}
			}
		}

		wglMakeCurrent(0, 0);
		wglDeleteContext(rc);
		ReleaseDC(hwnd, dc);
		DestroyWindow(hwnd);
		UnregisterClass(L"tmp", GetModuleHandle(0));
	}

	if (!wglChoosePixelFormatARB || !wglCreateContextAttribsARB || !wglSwapIntervalEXT || !has_srgb_framebuffer) {
		MessageBox(0, L"Please update your graphics card driver.", L"Missing OpenGL Function", MB_OK | MB_ICONERROR);
		ExitProcess(1);
	}

	////////
	//
	// main window.
	//
	RegisterClass(&(WNDCLASS){
		.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW,
		.lpfnWndProc = WindowProcMain,
		.hInstance = GetModuleHandle(0),
		.hIcon = LoadIcon(0, IDI_APPLICATION),
		.hCursor = LoadCursor(0, IDC_ARROW),
		.lpszClassName = L"main"
	});

	HWND hwnd = CreateWindowEx(0, L"main", L"Untitled", WS_OVERLAPPEDWINDOW,
				CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
				0, 0, GetModuleHandle(0), 0);
	HDC dc = GetDC(hwnd);

	const int pixel_attribues[] = {
		WGL_DRAW_TO_WINDOW_ARB, true,
		WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
		WGL_SUPPORT_OPENGL_ARB, true,
		WGL_DOUBLE_BUFFER_ARB, true,
		WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
		WGL_COLOR_BITS_ARB, 24,
		WGL_ALPHA_BITS_ARB, 8,
		WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, true,
		0 /* end */
	};
	int pf;
	UINT pixel_format_count;
    	wglChoosePixelFormatARB(dc, pixel_attribues, 0, 1, &pf, &pixel_format_count);
    	SetPixelFormat(dc, pf, &(PIXELFORMATDESCRIPTOR){ .nSize = sizeof(PIXELFORMATDESCRIPTOR), .nVersion = 1 });

    	const int context_attributes[] = {
		WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
		WGL_CONTEXT_MINOR_VERSION_ARB, 3,
		WGL_CONTEXT_LAYER_PLANE_ARB, 0,
		//WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
		0 /* end */
    	};
    	HGLRC rc = wglCreateContextAttribsARB(dc, 0, context_attributes);
	wglMakeCurrent(dc, rc);

	////////
	//
	// init opengl.
	//
	wglSwapIntervalEXT(1);
	reload_code();

	////////
	//
	// main loop.
	//
	ShowWindow(hwnd, SW_SHOWNORMAL);

	MSG msg;
	for (;;) {
		reload_code();

		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				ExitProcess(0);
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		WinRender(hwnd, dc);
	}
}
