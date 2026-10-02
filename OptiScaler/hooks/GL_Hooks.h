#pragma once

#include "SysUtils.h"

namespace GLHooks
{
// Hooks the present calls of the real opengl32.dll. Used on the opengl32.dll route.
void Hook(HMODULE opengl32);

// An OpenGL entry point of the real opengl32.dll, from wglGetProcAddress for extensions and the
// module's exports for OpenGL 1.1. Needs a current context for extensions.
PROC GetProc(const char* name);
} // namespace GLHooks
