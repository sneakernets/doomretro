/*
========================================================================

                               DOOM RETRO
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright (C) 2013-2015 Brad Harding.

  DOOM RETRO is a fork of CHOCOLATE DOOM by Simon Howard.
  For a complete list of credits, see the accompanying AUTHORS file.

  This file is part of DOOM RETRO.

  DOOM RETRO is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM RETRO is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM RETRO. If not, see <http://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM RETRO is in no way affiliated with nor endorsed by
  id Software LLC.

========================================================================
*/

#include <stdio.h>

#include "i_video.h"
#include "m_argv.h"
#include "m_misc.h"
#include "version.h"

#ifdef WIN32

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "d_main.h"
#include "m_argv.h"

#ifdef WIN32
#include "SDL_syswm.h"
#endif

void I_SetProcessPriority(HANDLE hProcess)
{
    SetPriorityClass(hProcess, ABOVE_NORMAL_PRIORITY_CLASS);
}

extern int      fullscreen;
extern boolean  window_focused;
HHOOK           g_hKeyboardHook;

void G_ScreenShot(void);

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    boolean     bEatKeystroke = false;

    if (nCode == HC_ACTION)
        switch (wParam)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (window_focused)
            {
                KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT *)lParam;

                if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN)
                    bEatKeystroke = true;
                else if (p->vkCode == VK_SNAPSHOT)
                {
                    if (wParam == WM_KEYDOWN)
                        G_ScreenShot();
                    bEatKeystroke = true;
                }
                }
            break;
        }

    return (bEatKeystroke ? 1 : CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam));
}

WNDPROC         oldProc;
HICON           icon;
HWND            hwnd;

boolean MouseShouldBeGrabbed(void);
void ToggleFullScreen(void);
void I_InitGamepad(void);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_SETCURSOR)
    {
        if (LOWORD(lParam) == HTCLIENT && !MouseShouldBeGrabbed())
        {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return true;
        }
    }
    else if (msg == WM_SYSCOMMAND)
    {
        if ((wParam & 0xfff0) == SC_MAXIMIZE)
        {
            ToggleFullScreen();
            return true;
        }
        else if ((wParam & 0xfff0) == SC_KEYMENU)
            return false;
    }
    else if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN && !(lParam & 0x40000000))
    {
        ToggleFullScreen();
        return true;
    }
    else if (msg == WM_DEVICECHANGE)
        I_InitGamepad();

    return CallWindowProc(oldProc, hwnd, msg, wParam, lParam);
}

HANDLE hInstanceMutex;

STICKYKEYS g_StartupStickyKeys = { sizeof(STICKYKEYS), 0 };
TOGGLEKEYS g_StartupToggleKeys = { sizeof(TOGGLEKEYS), 0 };
FILTERKEYS g_StartupFilterKeys = { sizeof(FILTERKEYS), 0 };

void I_AccessibilityShortcutKeys(boolean bAllowKeys)
{
    if (bAllowKeys)
    {
        // Restore StickyKeys/etc to original state
        SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &g_StartupStickyKeys, 0);
        SystemParametersInfo(SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS), &g_StartupToggleKeys, 0);
        SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_StartupFilterKeys, 0);
    }
    else
    {
        // Disable StickyKeys/etc shortcuts
        STICKYKEYS skOff = g_StartupStickyKeys;
        TOGGLEKEYS tkOff = g_StartupToggleKeys;
        FILTERKEYS fkOff = g_StartupFilterKeys;

        if ((skOff.dwFlags & SKF_STICKYKEYSON) == 0)
        {
            // Disable the hotkey and the confirmation
            skOff.dwFlags &= ~SKF_HOTKEYACTIVE;
            skOff.dwFlags &= ~SKF_CONFIRMHOTKEY;

            SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &skOff, 0);
        }

        if ((tkOff.dwFlags & TKF_TOGGLEKEYSON) == 0)
        {
            // Disable the hotkey and the confirmation
            tkOff.dwFlags &= ~TKF_HOTKEYACTIVE;
            tkOff.dwFlags &= ~TKF_CONFIRMHOTKEY;

            SystemParametersInfo(SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS), &tkOff, 0);
        }

        if ((fkOff.dwFlags & FKF_FILTERKEYSON) == 0)
        {
            // Disable the hotkey and the confirmation
            fkOff.dwFlags &= ~FKF_HOTKEYACTIVE;
            fkOff.dwFlags &= ~FKF_CONFIRMHOTKEY;

            SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &fkOff, 0);
        }
    }
}

#ifdef SDL20
extern SDL_Window       *window;
#endif

void I_InitWindows32(void)
{
    HINSTANCE           handle = GetModuleHandle(NULL);
    SDL_SysWMinfo       info;

    SDL_VERSION(&info.version);

#ifdef SDL20
    SDL_GetWindowWMInfo(window, &info);
    hwnd = info.info.win.window;
#else
    SDL_GetWMInfo(&info);
    hwnd = info.window;
#endif

    icon = LoadIcon(handle, "IDI_ICON1");
    SetClassLongPtr(hwnd, GCLP_HICON, (LONG)icon);

    oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG)WndProc);
}

void I_ShutdownWindows32(void)
{
    DestroyIcon(icon);
    UnhookWindowsHookEx(g_hKeyboardHook);
    ReleaseMutex(hInstanceMutex);
    CloseHandle(hInstanceMutex);
    I_AccessibilityShortcutKeys(true);
}
#endif

int main(int argc, char **argv)
{
#ifdef WIN32
    HANDLE hProcess = GetCurrentProcess();

    hInstanceMutex = CreateMutex(NULL, true, PACKAGE_MUTEX);

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hInstanceMutex)
            CloseHandle(hInstanceMutex);
        SetForegroundWindow(FindWindow(PACKAGE_MUTEX, NULL));
        return 1;
    }

    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);

    // Save the current sticky/toggle/filter key settings so they can be restored them later
    SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), &g_StartupStickyKeys, 0);
    SystemParametersInfo(SPI_GETTOGGLEKEYS, sizeof(TOGGLEKEYS), &g_StartupToggleKeys, 0);
    SystemParametersInfo(SPI_GETFILTERKEYS, sizeof(FILTERKEYS), &g_StartupFilterKeys, 0);

    I_AccessibilityShortcutKeys(false);
#endif

    myargc = argc;
    myargv = argv;

#ifdef WIN32
    if (!M_CheckParm("-nopriority"))
        I_SetProcessPriority(hProcess);
#endif

    D_DoomMain();

    return 0;
}
