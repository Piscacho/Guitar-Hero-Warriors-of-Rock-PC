// Prueba del traductor (sesion 8): GH6_SHADERTEST=<carpeta> traduce y compila cada .bin y sale.
#include "gpu_shader.h"
#include "runtime.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdio>

int RunShaderTest(const char* dir)
{
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((std::string(dir) + "/*.bin").c_str(), &fd);
    int ok = 0, total = 0;
    if (h == INVALID_HANDLE_VALUE) { LOG("[shadertest] sin .bin en %s", dir); return 1; }
    CreateDirectoryA("shaders_hlsl", nullptr);
    do
    {
        std::string path = std::string(dir) + "/" + fd.cFileName;
        FILE* f = fopen(path.c_str(), "rb"); if (!f) continue;
        std::vector<uint32_t> w; uint32_t be;
        while (fread(&be, 4, 1, f) == 1) w.push_back(__builtin_bswap32(be));
        fclose(f);
        bool ps = fd.cFileName[0] == 'p';
        total++;
        auto t = TranslateXenosShader(ps ? 1 : 0, w.data(), (uint32_t)w.size());
        std::string out = std::string("shaders_hlsl/") + fd.cFileName + ".hlsl";
        if (FILE* o = fopen(out.c_str(), "w")) { fputs(t.hlsl.c_str(), o); fclose(o); }
        if (!t.ok) { LOG("[shadertest] %s: NO TRADUCIDO (%s)", fd.cFileName, t.error.c_str()); continue; }
        Microsoft::WRL::ComPtr<ID3DBlob> code, err;
        HRESULT hr = D3DCompile(t.hlsl.data(), t.hlsl.size(), fd.cFileName, nullptr, nullptr, "main", ps ? "ps_5_0" : "vs_5_0", 0, 0, &code, &err);
        if (FAILED(hr)) { LOG("[shadertest] %s: ERROR DE COMPILACION\n%.600s", fd.cFileName, err ? (const char*)err->GetBufferPointer() : "?"); continue; }
        ok++;
        LOG("[shadertest] %s: OK (%zu bytes DXBC, streams %zu, texturas 0x%X)", fd.cFileName, code->GetBufferSize(), t.vertexFetchSlots.size(), t.textureMask);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    LOG("[shadertest] %d/%d shaders compilados", ok, total);
    return ok == total ? 0 : 2;
}
