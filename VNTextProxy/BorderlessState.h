#pragma once

#include <windows.h>

// Shared state for borderless fullscreen implementation
// Accessed by both D3D9Hooks and Win32AToWAdapter

namespace BorderlessState
{
    // Is borderless mode currently active?
    inline bool g_borderlessActive = false;

    // Main game window handle (tracked by CreateWindowExA hook)
    inline HWND g_mainGameWindow = nullptr;

    // Original game resolution (what the game thinks it's rendering at)
    inline int g_gameWidth = 800;
    inline int g_gameHeight = 600;

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
    }

    // Calculate pillarbox/letterbox dimensions
    inline void CalculateScaling()
    {
        if (g_screenWidth == 0 || g_screenHeight == 0)
            GetNativeResolution();

        float gameAspect = (float)g_gameWidth / (float)g_gameHeight;  // 4:3 = 1.333
        float screenAspect = (float)g_screenWidth / (float)g_screenHeight;

        if (screenAspect > gameAspect)
        {
            // Screen is wider than game - add pillarboxes (black bars on sides)
            g_scaledHeight = g_screenHeight;
            g_scaledWidth = (int)(g_screenHeight * gameAspect);
            g_offsetX = (g_screenWidth - g_scaledWidth) / 2;
            g_offsetY = 0;
        }
        else
        {
            // Screen is taller than game - add letterboxes (black bars top/bottom)
            g_scaledWidth = g_screenWidth;
            g_scaledHeight = (int)(g_screenWidth / gameAspect);
            g_offsetX = 0;
            g_offsetY = (g_screenHeight - g_scaledHeight) / 2;
        }
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
