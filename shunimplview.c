/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <shlobj.h>
#include <uxtheme.h>
#include "resource.h"

#define RECTWIDTH(rc)  ((rc).right - (rc).left)
#define RECTHEIGHT(rc) ((rc).bottom - (rc).top)

#define IMAGE_NAME_RVA64(Ordinal) (Ordinal & 0x7fffffffull)
#define IMAGE_NAME_RVA32(Ordinal) (Ordinal & 0x7fffffff)
#ifdef _WIN64
#  define IMAGE_NAME_RVA IMAGE_NAME_RVA64
#else
#  define IMAGE_NAME_RVA IMAGE_NAME_RVA32
#endif

LPCWSTR kMainWndClassName = L"ShunimplView";

HINSTANCE g_hInst             = NULL;
HACCEL    g_hAccel            = NULL;
HMODULE   g_hLoadedModule     = NULL;
DWORD     g_dwFunctions       = 0;
HWND      g_hWnd              = NULL;
HWND      g_hWndTree          = NULL;
HWND      g_hWndStatus        = NULL;
ULONGLONG g_uShunimplBase     = 0;
ULONGLONG g_uShunimplSize     = 0;

void SetTitleText(LPCWSTR lpFileName)
{
	WCHAR szTitleText[256];
	LoadStringW(g_hInst, lpFileName ? IDS_MAINWND_LOADED : IDS_MAINWND, szTitleText, 256);
	if (lpFileName)
	{
		WCHAR szBuffer[256];
		swprintf_s(szBuffer, 256, szTitleText, lpFileName);
		wcscpy_s(szTitleText, 256, szBuffer);
	}
	SetWindowTextW(g_hWnd, szTitleText);
}

void UnloadFile(void)
{
	if (g_hLoadedModule)
	{
		FreeLibrary(g_hLoadedModule);
		g_hLoadedModule = NULL;
	}
	g_dwFunctions = 0;
	TreeView_DeleteAllItems(g_hWndTree);
	SendMessageW(g_hWndStatus, SB_SETTEXT, NULL, (LPARAM)L"");
}

void SizeWindows(void)
{
	SendMessageW(g_hWndStatus, WM_SIZE, 0, 0);

	RECT rcWindow, rcStatus;
	GetClientRect(g_hWnd, &rcWindow);
	GetWindowRect(g_hWndStatus, &rcStatus);

	SetWindowPos(
		g_hWndTree,
		NULL,
		0, 0,
		RECTWIDTH(rcWindow),
		RECTHEIGHT(rcWindow) - RECTHEIGHT(rcStatus),
		SWP_NOMOVE | SWP_NOZORDER
	);
}

HTREEITEM AddTreeItem(HTREEITEM hParent, LPSTR lpName)
{
	TVINSERTSTRUCTA tvis = { 0 };
	tvis.hParent = hParent;
	tvis.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
	tvis.item.iImage = Shell_GetCachedImageIndexW(L"imageres.dll", hParent ? -170 : -67, NULL);
	tvis.item.iSelectedImage = tvis.item.iImage;
	tvis.item.pszText = lpName;
	tvis.item.cchTextMax = strlen(lpName) + 1;
	HTREEITEM hTreeItem = SendMessageW(g_hWndTree, TVM_INSERTITEMA, NULL, (LPARAM)&tvis);
	// For some reason, you can only set an item to the expanded state when it has children.
	// Just expand it whenever adding a child.
	if (hParent)
		TreeView_Expand(g_hWndTree, hParent, TVE_EXPAND);
	return hTreeItem;
}

BOOL PopulateTree(void)
{
	LPBYTE lpBase = (LPBYTE)g_hLoadedModule;
	PIMAGE_DOS_HEADER pDOSHead = (PIMAGE_DOS_HEADER)lpBase;
	PIMAGE_NT_HEADERS pNTHead = (PIMAGE_NT_HEADERS)(lpBase + pDOSHead->e_lfanew);
	PIMAGE_IMPORT_DESCRIPTOR pImportDir =
		(PIMAGE_IMPORT_DESCRIPTOR)(lpBase + pNTHead->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	while (pImportDir->Name)
	{
		LPSTR lpDllName = (LPSTR)(lpBase + pImportDir->Name);
		HMODULE hDll = LoadLibraryA(lpDllName);
		if (!hDll)
		{
			pImportDir++;
			continue;
		}
		PDWORD_PTR pImportThunk = (PDWORD_PTR)(lpBase + pImportDir->OriginalFirstThunk);
		HTREEITEM hModuleTreeItem = NULL;
		while (*pImportThunk)
		{
			DWORD_PTR dwThunk = *pImportThunk;
			if (dwThunk & IMAGE_ORDINAL_FLAG)
			{
				ULONGLONG uFuncBase = (ULONGLONG)GetProcAddress(hDll, (LPCSTR)IMAGE_ORDINAL(dwThunk));
				if (uFuncBase >= g_uShunimplBase && uFuncBase <= (g_uShunimplBase + g_uShunimplSize))
				{
					if (!hModuleTreeItem)
						hModuleTreeItem = AddTreeItem(NULL, lpDllName);

					char szItemText[256];
					sprintf_s(szItemText, 256, "Ordinal #%u", IMAGE_ORDINAL(dwThunk));
					AddTreeItem(hModuleTreeItem, szItemText);
					g_dwFunctions++;
				}
			}
			else
			{
				PIMAGE_IMPORT_BY_NAME pByName = (PIMAGE_IMPORT_BY_NAME)(lpBase + IMAGE_NAME_RVA(dwThunk));
				ULONGLONG uFuncBase = (ULONGLONG)GetProcAddress(hDll, pByName->Name);
				if (uFuncBase >= g_uShunimplBase && uFuncBase <= (g_uShunimplBase + g_uShunimplSize))
				{
					if (!hModuleTreeItem)
						hModuleTreeItem = AddTreeItem(NULL, lpDllName);

					AddTreeItem(hModuleTreeItem, pByName->Name);
					g_dwFunctions++;
				}
			}
			pImportThunk++;
		}
		FreeLibrary(hDll);
		pImportDir++;
	}

	return TRUE;
}

void BrowseForModule(void)
{
	IFileOpenDialog *pfd = NULL;
	IShellItem *psi = NULL;
	LPWSTR pszFileName = NULL;
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, &pfd)))
		return;

	COMDLG_FILTERSPEC spec[] = {
		{ L"Portable Executables (*.exe, *.dll)", L"*.exe;*.dll" },
		{ L"All Files (*.*)", L"*.*" }
	};
	pfd->lpVtbl->SetFileTypes(pfd, 2, spec);

	HRESULT hr = pfd->lpVtbl->Show(pfd, g_hWnd);
	if (FAILED(hr))
		goto cleanup;

	if (FAILED(pfd->lpVtbl->GetResult(pfd, &psi)))
		goto cleanup;

	if (FAILED(psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &pszFileName)))
		goto cleanup;

	UnloadFile();

	g_hLoadedModule = LoadLibraryExW(pszFileName, NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (!g_hLoadedModule)
	{
		MessageBoxW(g_hWnd, L"Failed to load that file. Does the machine type match?", L"ShunimplView", MB_ICONERROR);
		SetTitleText(NULL);
		goto cleanup;
	}

	if (!PopulateTree())
		goto cleanup;

	SetTitleText(PathFindFileNameW(pszFileName));

	WCHAR szFunctionCount[256];
	swprintf_s(szFunctionCount, 256, L"%u function(s)", g_dwFunctions);
	SendMessageW(g_hWndStatus, SB_SETTEXT, NULL, szFunctionCount);
	TreeView_EnsureVisible(g_hWndTree, TreeView_GetRoot(g_hWndTree));
	
cleanup:
	if (pfd)
		pfd->lpVtbl->Release(pfd);
	if (psi)
		psi->lpVtbl->Release(psi);
	if (pszFileName)
		CoTaskMemFree(pszFileName);
}

void UpdateTreeStyle(void)
{
	// Hot tracking looks bad on classic theme
	HTHEME hTheme = OpenThemeData(g_hWndTree, L"TreeView");
	DWORD dwStyle = GetWindowLongPtrW(g_hWndTree, GWL_STYLE);
	if (hTheme)
	{
		dwStyle |= TVS_TRACKSELECT;
		CloseThemeData(hTheme);
	}
	else
	{
		dwStyle &= ~TVS_TRACKSELECT;
	}
	SetWindowLongPtrW(g_hWndTree, GWL_STYLE, dwStyle);
}

LRESULT CALLBACK TreeSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	switch (uMsg)
	{
		// Prevent ugly link cursor from tree view hot tracking
		case WM_SETCURSOR:
			SetCursor(LoadCursorW(NULL, IDC_ARROW));
			return 0;
		default:
			return DefSubclassProc(hWnd, uMsg, wParam, lParam);
	}
}

INT_PTR CALLBACK AboutDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			WCHAR szFormat[256], szBuffer[256];
			GetDlgItemTextW(hWnd, IDC_VERSION, szFormat, 256);
			swprintf_s(
				szBuffer, 256, szFormat,
				VER_MAJOR, VER_MINOR, VER_REVISION
			);
			SetDlgItemTextW(hWnd, IDC_VERSION, szBuffer);
			return TRUE;
		}
		case WM_CLOSE:
			EndDialog(hWnd, IDCANCEL);
			return TRUE;
		case WM_COMMAND:
			switch (wParam)
			{
				case IDC_GITHUB:
					ShellExecuteW(
						hWnd, L"open",
						L"https://github.com/aubymori/ShunimplView",
						NULL, NULL,
						SW_SHOWNORMAL
					);
					// fall-thru
				case IDOK:
				case IDCANCEL:
					EndDialog(hWnd, wParam);
			}
			return TRUE;
		default:
			return FALSE;
	}
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_CREATE:
			g_hWnd = hWnd;
			SetTitleText(NULL);
			SizeWindows();
			return 0;
		case WM_CLOSE:
			DestroyWindow(hWnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_SIZE:
			SizeWindows();
			return 0;
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDM_FILEOPEN:
					BrowseForModule();
					break;
				case IDM_FILECLOSE:
					UnloadFile();
					SetTitleText(NULL);
					break;
				case IDM_FILEEXIT:
					SendMessageW(hWnd, WM_CLOSE, 0, 0);
					break;
				case IDM_HELPABOUT:
					DialogBoxParamW(
						g_hInst, MAKEINTRESOURCEW(IDD_ABOUT),
						hWnd, AboutDlgProc, NULL
					);
					break;
			}
			return 0;
		case WM_HOTKEY:
			SendMessageW(hWnd, WM_COMMAND, wParam, 0);
			return 0;
		default:
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}
}

int WINAPI wWinMain(
	_In_     HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_     LPWSTR    lpCmdLine,
	_In_     int       nShowCmd
)
{
	g_hInst = hInstance;
	g_hAccel = LoadAcceleratorsW(g_hInst, MAKEINTRESOURCEW(IDA_MAIN));

	WCHAR szModulePath[MAX_PATH];
	GetSystemDirectoryW(szModulePath, MAX_PATH);
	PathAppendW(szModulePath, L"shunimpl.dll");
	AddAtomW(L"FailObsoleteShellAPIs");
	HMODULE hShunimpl = LoadLibraryW(szModulePath);
	if (!hShunimpl)
	{
		MessageBoxW(NULL, L"Failed to load shunimpl.dll.", L"ShunimplView", MB_ICONERROR);
		return 1;
	}

	MODULEINFO mi;
	GetModuleInformation(GetCurrentProcess(), hShunimpl, &mi, sizeof(MODULEINFO));
	g_uShunimplBase = hShunimpl;
	g_uShunimplSize = mi.SizeOfImage;

	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = MainWndProc;
	wc.hInstance = g_hInst;
	wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_SHUNIMPLVIEW));
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (COLOR_WINDOW + 1);
	wc.lpszMenuName = MAKEINTRESOURCEW(IDM_MAINMENU);
	wc.lpszClassName = kMainWndClassName;
	RegisterClassW(&wc);

	CreateWindowExW(
		NULL,
		kMainWndClassName,
		NULL,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		600, 400,
		NULL, NULL, g_hInst, NULL
	);

	g_hWndTree = CreateWindowExW(
		NULL,
		L"SysTreeView32", NULL,
		WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_LINESATROOT,
		0, 0, 0, 0,
		g_hWnd, NULL, NULL,
		NULL
	);
	SetWindowTheme(g_hWndTree, L"Explorer", NULL);
	UpdateTreeStyle();
	SetWindowSubclass(g_hWndTree, TreeSubclassProc, 0, 0);

	HIMAGELIST himl, himlSmall;
	Shell_GetImageLists(&himl, &himlSmall);
	TreeView_SetImageList(g_hWndTree, himlSmall, TVSIL_NORMAL);

	g_hWndStatus = CreateWindowExW(
		NULL,
		L"msctls_statusbar32", NULL,
		WS_CHILD | WS_VISIBLE,
		0, 0, 0, 0,
		g_hWnd, NULL, NULL,
		NULL
	);

	ShowWindow(g_hWnd, nShowCmd);
	UpdateWindow(g_hWnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0))
	{
		if (!TranslateAcceleratorW(g_hWnd, g_hAccel, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	return 0;
}