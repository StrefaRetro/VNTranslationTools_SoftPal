#pragma once

#include <windows.h>
#include <cstdio>
#include <share.h>
#include "Util/RuntimeConfig.h"

// Shared state for pillarboxed fullscreen implementation
// Accessed by both D3D9Hooks and Win32AToWAdapter

namespace PillarboxedState
{
    // Logging support
    inline FILE* g_pillarboxLog = nullptr;

    inline void pillarbox_log(const char* format, ...)
    {
        if (!RuntimeConfig::DebugLogging())
            return;
        if (!g_pillarboxLog)
            g_pillarboxLog = _fsopen("./pillarbox.log", "w", _SH_DENYNO);
        if (g_pillarboxLog)
        {
            va_list args;
            va_start(args, format);
            vfprintf(g_pillarboxLog, format, args);
            fprintf(g_pillarboxLog, "\n");
            va_end(args);
            fflush(g_pillarboxLog);
        }
    }
    // Is pillarboxed mode currently active?
    inline bool g_pillarboxedActive = false;

    // Main game window handle (tracked by CreateWindowExA hook)
    inline HWND g_mainGameWindow = nullptr;

    // Original game resolution (what the game thinks it's rendering at)
    // Set from SetViewport hooks to get true resolution unaffected by DPI virtualization
    inline int g_gameWidth = 800;
    inline int g_gameHeight = 600;

    // Called from SetViewport hooks to capture true game resolution
    inline void SetGameResolution(int width, int height)
    {
        g_gameWidth = width;
        g_gameHeight = height;
    }

    // Native screen resolution (what we're actually presenting at)
    inline int g_screenWidth = 0;
    inline int g_screenHeight = 0;

    // Calculated scaling parameters for pillarboxing
    inline int g_scaledWidth = 0;
    inline int g_scaledHeight = 0;
    inline int g_offsetX = 0;
    inline int g_offsetY = 0;

    // Get native screen resolution
    inline void GetNativeResolution()
    {
        g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
        g_screenHeight = GetSystemMetrics(SM_CYSCREEN);
        pillarbox_log("GetNativeResolution: SM_CXSCREEN=%d, SM_CYSCREEN=%d", g_screenWidth, g_screenHeight);

        // Also log info about monitors
        int numMonitors = GetSystemMetrics(SM_CMONITORS);
        int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        pillarbox_log("  NumMonitors=%d, VirtualScreen=(%d,%d) %dx%d",
            numMonitors, virtualX, virtualY, virtualWidth, virtualHeight);
    }

    // Calculate pillarbox/letterbox dimensions
    inline void CalculateScaling()
    {
        pillarbox_log("CalculateScaling: ENTRY - game=%dx%d, screen=%dx%d",
            g_gameWidth, g_gameHeight, g_screenWidth, g_screenHeight);

        if (g_screenWidth == 0 || g_screenHeight == 0)
        {
            pillarbox_log("  Screen dimensions zero, calling GetNativeResolution...");
            GetNativeResolution();
        }

        float gameAspect = (float)g_gameWidth / (float)g_gameHeight;  // 4:3 = 1.333
        float screenAspect = (float)g_screenWidth / (float)g_screenHeight;

        pillarbox_log("  gameAspect=%.4f (%d/%d), screenAspect=%.4f (%d/%d)",
            gameAspect, g_gameWidth, g_gameHeight,
            screenAspect, g_screenWidth, g_screenHeight);

        if (screenAspect > gameAspect)
        {
            // Screen is wider than game - add pillarboxes (black bars on sides)
            pillarbox_log("  PILLARBOX mode: screen wider than game (%.4f > %.4f)", screenAspect, gameAspect);
            g_scaledHeight = g_screenHeight;
            g_scaledWidth = (int)(g_screenHeight * gameAspect);
            g_offsetX = (g_screenWidth - g_scaledWidth) / 2;
            g_offsetY = 0;
            pillarbox_log("  scaledWidth = screenHeight * gameAspect = %d * %.4f = %d",
                g_screenHeight, gameAspect, g_scaledWidth);
        }
        else
        {
            // Screen is taller than game - add letterboxes (black bars top/bottom)
            pillarbox_log("  LETTERBOX mode: screen taller than game (%.4f <= %.4f)", screenAspect, gameAspect);
            g_scaledWidth = g_screenWidth;
            g_scaledHeight = (int)(g_screenWidth / gameAspect);
            g_offsetX = 0;
            g_offsetY = (g_screenHeight - g_scaledHeight) / 2;
            pillarbox_log("  scaledHeight = gameHeight * (scaledWidth/gameWidth) = %d * (%d/%d) = %d",
                g_gameHeight, g_scaledWidth, g_gameWidth, g_scaledHeight);
        }

        pillarbox_log("  RESULT: scaled=%dx%d, offset=(%d,%d)",
            g_scaledWidth, g_scaledHeight, g_offsetX, g_offsetY);
        pillarbox_log("  Rendering area: (%d,%d) to (%d,%d)",
            g_offsetX, g_offsetY, g_offsetX + g_scaledWidth, g_offsetY + g_scaledHeight);
    }

    // Transform game coordinates (800x600) to screen coordinates (with pillarboxing)
    inline void GameToScreen(int gameX, int gameY, int& screenX, int& screenY)
    {
        // Scale from game resolution to scaled area, then add offset
        screenX = g_offsetX + (gameX * g_scaledWidth / g_gameWidth);
        screenY = g_offsetY + (gameY * g_scaledHeight / g_gameHeight);
    }

    // Transform screen coordinates to game coordinates
    inline void ScreenToGame(int screenX, int screenY, int& gameX, int& gameY)
    {
        // Remove offset, then scale from scaled area to game resolution
        gameX = (screenX - g_offsetX) * g_gameWidth / g_scaledWidth;
        gameY = (screenY - g_offsetY) * g_gameHeight / g_scaledHeight;

        // Clamp to game bounds
        if (gameX < 0) gameX = 0;
        if (gameX >= g_gameWidth) gameX = g_gameWidth - 1;
        if (gameY < 0) gameY = 0;
        if (gameY >= g_gameHeight) gameY = g_gameHeight - 1;
    }

    // Transform a game-space RECT to screen-space RECT
    inline void GameRectToScreen(const RECT& gameRect, RECT& screenRect)
    {
        GameToScreen(gameRect.left, gameRect.top, (int&)screenRect.left, (int&)screenRect.top);
        GameToScreen(gameRect.right, gameRect.bottom, (int&)screenRect.right, (int&)screenRect.bottom);
    }
}
