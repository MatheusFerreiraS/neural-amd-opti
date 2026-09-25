@echo off
rem Build MochizukiNrRuntime.dll and its dlssnr-amd\shaders from third_party\mochizuki.
rem Run from an MSVC developer prompt. Needs the Vulkan SDK 1.4.357 or newer (VULKAN_SDK) and Python;
rem glslang 16.5.0, the version the upstream shaders are pinned to, is downloaded on first use.
setlocal
cd /d "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=exports\mochizuki-runtime"
set "UP=third_party\mochizuki"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"

where cl.exe >nul 2>&1
if errorlevel 1 (
  echo FAIL: cl.exe not found; run from an MSVC developer prompt
  exit /b 1
)
if not defined VULKAN_SDK (
  echo FAIL: VULKAN_SDK is not set; install the Vulkan SDK 1.4.357 or newer
  exit /b 1
)

rem The network's constants, from %UP%\windows\build\arch\rdna4.sh. The shaders are built with the same ones.
set "NR_DEFINES=/DNR_GEMM_WIDE_MT=64 /DNR_GEMM_WIDE_NT=256 /DNR_GEMM_PROJ_MT=32 /DNR_GEMM_PROJ_NT=128 /DNR_PROJW_MIN_TOKENS=2048 /DNR_FFWD_WGW=4 /DNR_UPSVIEW_VEC=1 /DNR_REPACK_VEC=1 /DNR_WIDE_UPS_MASK=7 /DNR_PERSIST_DF=1 /DNR_DS_FUSE=15 /DNR_DECUPS_VEC=16 /DNR_DECQ_SMALL=1 /DNR_PERSIST_DS_MASK=4 /DNR_PERSIST_UPS_MASK=4"
set "CFLAGS=/nologo /std:c++20 /O2 /EHsc /MT /utf-8 /bigobj /W3 /wd4244 /wd4267 /wd4305 /wd4018 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DNDEBUG"
set "INCLUDES=/I "%VULKAN_SDK%\Include" /I "%UP%\windows\src\core" /I "OptiScaler\dlssnr\backend\mochizuki_runtime\compat" /I "OptiScaler\dlssnr\backend\lmxxf_runtime""

rem The core's pipeline creation goes through mz_interpose.cpp (the parallel prewarm and its manifest): these six
rem entry points are renamed for nr_runtime.cpp alone, which calls them directly, never through vkGetDeviceProcAddr.
set "MZI=/DvkCreateComputePipelines=mzi_vkCreateComputePipelines /DvkCreateShaderModule=mzi_vkCreateShaderModule /DvkCreateDescriptorSetLayout=mzi_vkCreateDescriptorSetLayout /DvkCreatePipelineLayout=mzi_vkCreatePipelineLayout /DvkCreatePipelineCache=mzi_vkCreatePipelineCache /DvkDestroyPipelineCache=mzi_vkDestroyPipelineCache"

cl %CFLAGS% %NR_DEFINES% %MZI% %INCLUDES% /c "%UP%\windows\src\core\nr_runtime.cpp" /Fo"%OBJ%\nr_runtime.obj" || goto fail
cl %CFLAGS% %INCLUDES% /c "%UP%\windows\src\core\nr_native_plan.cpp" /Fo"%OBJ%\nr_native_plan.obj" || goto fail
cl %CFLAGS% %INCLUDES% /c "OptiScaler\dlssnr\backend\mochizuki_runtime\mz_interpose.cpp" /Fo"%OBJ%\mz_interpose.obj" || goto fail
cl %CFLAGS% %INCLUDES% /DLMXXF_NR_RUNTIME_EXPORTS /c "OptiScaler\dlssnr\backend\mochizuki_runtime\MochizukiNrRuntime.cpp" /Fo"%OBJ%\MochizukiNrRuntime.obj" || goto fail
link /nologo /DLL /OUT:"%OUT%\MochizukiNrRuntime.dll" "%OBJ%\MochizukiNrRuntime.obj" "%OBJ%\mz_interpose.obj" "%OBJ%\nr_runtime.obj" "%OBJ%\nr_native_plan.obj" "%VULKAN_SDK%\Lib\vulkan-1.lib" d3d12.lib || goto fail

set "GLSLANG=%UP%\toolchain\glslang"
if not exist "%GLSLANG%\bin\glslang.exe" (
  if not exist "%UP%\toolchain" mkdir "%UP%\toolchain"
  python -c "import hashlib,io,urllib.request,zipfile; b=urllib.request.urlopen('https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip').read(); assert hashlib.sha256(b).hexdigest()=='06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce', 'glslang checksum mismatch'; zipfile.ZipFile(io.BytesIO(b)).extractall(r'%GLSLANG%')" || goto fail
)
python "%UP%\windows\build\build_network.py" rdna4 --out "%OUT%\dlssnr-amd\shaders" || goto fail

echo BUILD_OK %OUT%\MochizukiNrRuntime.dll
exit /b 0

:fail
echo FAIL: MochizukiNrRuntime build failed
exit /b 1
