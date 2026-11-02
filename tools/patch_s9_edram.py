"""Sesion 9: EDRAM (un RT por superficie), resolves a memoria y presentacion del frontbuffer real."""
p = "G:/Guitar Hero Warrior of rock/Recomp/runtime/gpu_d3d12.cpp"
s = open(p, encoding="utf-8").read()


def rep(old, new):
    global s
    assert old in s, old[:80]
    s = s.replace(old, new, 1)


rep("hd.NumDescriptors = kFrames + 1;", "hd.NumDescriptors = kFrames + 1 + 64;")
rep("ComPtr<ID3DBlob> g_rectGs;", """ComPtr<ID3DBlob> g_rectGs;
// ---- EDRAM (sesion 9): un RT por (base, pitch) ----
struct EdramRt { ComPtr<ID3D12Resource> res; UINT w = 0, h = 0, rtv = 0; };
std::unordered_map<uint32_t, EdramRt> g_edramRts;
EdramRt* g_boundRt = nullptr;
ComPtr<ID3D12Resource> g_readback;
uint64_t g_resolves = 0, g_fbResolves = 0;
constexpr UINT kEdramH = 1024;""")
rep("""    const float black[4] = { 0, 0, 0, 1 };
    g_list->ClearRenderTargetView(Rtv(kFrames), black, 0, nullptr);
    g_frameOpen = true;""", """    g_boundRt = nullptr;
    g_frameOpen = true;""")

rep("void D3D12Draw(const XenosDraw& d)\n{", r'''EdramRt* GetEdramRt()
{
    uint32_t base = Reg(0x2001) & 0xFFF, pitch = Reg(0x2000) & 0x3FFF;
    if (!pitch) pitch = kRtW;
    uint32_t key = (base << 16) | pitch;
    auto found = g_edramRts.find(key);
    if (found != g_edramRts.end()) return &found->second;
    if (g_edramRts.size() >= 60) return nullptr;
    EdramRt& rt = g_edramRts[key];
    rt.w = pitch; rt.h = kEdramH; rt.rtv = kFrames + 1 + (UINT)g_edramRts.size();
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = rt.w; td.Height = rt.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_B8G8R8A8_UNORM, { 0, 0, 0, 1 } };
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt.res)))) { g_edramRts.erase(key); return nullptr; }
    g_dev->CreateRenderTargetView(rt.res.Get(), nullptr, Rtv(rt.rtv));
    const float black[4] = { 0, 0, 0, 1 };
    g_list->ClearRenderTargetView(Rtv(rt.rtv), black, 0, nullptr);
    LOG("[edram] nueva superficie: base %u tiles, pitch %u", base, pitch);
    return &rt;
}

void FlushAndWait()
{
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence.Get(), g_nextFence);
    g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 5000);
    g_alloc[g_fi]->Reset();
    g_list->Reset(g_alloc[g_fi].Get(), nullptr);
    g_boundRt = nullptr;
}

// Resolve: copia un rectangulo de la superficie EDRAM actual a memoria fisica (tiled), y clear opcional.
void Resolve()
{
    uint32_t ctrl = Reg(0x2318), destBase = Reg(0x2319), destPitchReg = Reg(0x231A), destInfo = Reg(0x231B);
    uint32_t src = ctrl & 7, cmd = (ctrl >> 20) & 3;
    bool colorClear = (ctrl >> 8) & 1;
    // Rectangulo: 3 vertices float2 del stream vf0 del propio draw
    uint32_t vdw0 = Reg(0x4800), vdw1 = Reg(0x4801);
    const uint8_t* vb = g_base + (0xA0000000u | (vdw0 & 0x1FFFFFFC));
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 3; i++)
    {
        uint32_t ux = ((const uint32_t*)vb)[i * 2], uy = ((const uint32_t*)vb)[i * 2 + 1];
        if ((vdw1 & 3) == 2) { ux = __builtin_bswap32(ux); uy = __builtin_bswap32(uy); }
        float fx, fy; memcpy(&fx, &ux, 4); memcpy(&fy, &uy, 4);
        minx = std::min(minx, fx); maxx = std::max(maxx, fx); miny = std::min(miny, fy); maxy = std::max(maxy, fy);
    }
    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    int x0 = std::max(0, (int)minx), y0 = std::max(0, (int)miny);
    int x1 = std::min((int)rt->w, (int)maxx), y1 = std::min((int)rt->h, (int)maxy);
    uint32_t destPitch = destPitchReg & 0x3FFF, destH = (destPitchReg >> 16) & 0x3FFF;
    uint32_t fmt = (destInfo >> 7) & 0x3F, endian = destInfo & 7; bool swapRB = (destInfo >> 24) & 1;
    int w = x1 - x0, h = y1 - y0;
    static int logged = 0;
    if (logged < 12) { logged++; LOG("[edram] resolve src=%u cmd=%u rect=(%d,%d)-(%d,%d) -> %08X pitch %u alto %u fmt %u endian %u swap %d clear %d",
                                     src, cmd, x0, y0, x1, y1, destBase, destPitch, destH, fmt, endian, (int)swapRB, (int)colorClear); }
    if (w > 0 && h > 0 && src < 4 && cmd != 3 && fmt == 6 && destPitch)
    {
        if (!g_readback)
        {
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048ull * 4 * kEdramH; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
        }
        UINT rowPitch = ((UINT)w * 4 + 255) & ~255u;
        auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); g_list->ResourceBarrier(1, &a);
        D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, (UINT)w, (UINT)h, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION sl{ rt->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
        D3D12_BOX box{ (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, &box);
        auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); g_list->ResourceBarrier(1, &b);
        FlushAndWait();
        uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
        uint8_t* out = g_base + (0xA0000000u | (destBase & 0x1FFFFFFF));
        uint32_t pitchAligned = (destPitch + 31) & ~31u;
        uint32_t maxH = destH ? destH : 0x3FFF;
        for (int y = 0; y < h && (uint32_t)y < maxH; y++)
        {
            uint32_t outer = TiledOuter((uint32_t)y, pitchAligned, 2);
            for (int x = 0; x < w && (uint32_t)x < destPitch; x++)
            {
                const uint8_t* px = p + y * rowPitch + x * 4; // B G R A
                uint32_t v = swapRB ? (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24))   // bytes B G R A
                                    : (px[2] | (px[1] << 8) | (px[0] << 16) | (px[3] << 24));  // bytes R G B A
                *(uint32_t*)(out + TiledInner((uint32_t)x, (uint32_t)y, 2, outer)) = endian <= 3 ? Swap32(v, endian) : v;
            }
        }
        g_readback->Unmap(0, nullptr);
        g_resolves++;
        if ((destBase & 0x1FFFFFFF) == (GpuGetStats().frontbuffer.load() & 0x1FFFFFFF) || destBase == 0x17DF0000) g_fbResolves++;
    }
    if (colorClear && w > 0 && h > 0)
    {
        uint32_t c = Reg(0x231E);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        D3D12_RECT r{ x0, y0, x1, y1 };
        g_list->ClearRenderTargetView(Rtv(rt->rtv), col, 1, &r);
    }
}

void D3D12Draw(const XenosDraw& d)
{''')
rep("""    if ((Reg(0x2208) & 7) == 6) { g_skipped[4]++; return; }""", """    if ((Reg(0x2208) & 7) == 6) { Resolve(); return; }""")
rep("""    D3D12_CPU_DESCRIPTOR_HANDLE edramRtv = Rtv(kFrames);
    g_list->OMSetRenderTargets(1, &edramRtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, (float)kRtW, (float)kRtH, 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)kRtW, (LONG)kRtH };""", """    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    D3D12_CPU_DESCRIPTOR_HANDLE edramRtv = Rtv(rt->rtv);
    g_list->OMSetRenderTargets(1, &edramRtv, FALSE, nullptr);
    g_boundRt = rt;
    D3D12_VIEWPORT vp{ 0, 0, (float)rt->w, (float)rt->h, 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)rt->w, (LONG)rt->h };""")
rep("""        v[0] = (float)Reg(0x2206); v[1] = (float)kRtW; v[2] = (float)kRtH; v[3] = 0;""", """        uint32_t rtw = Reg(0x2000) & 0x3FFF; if (!rtw) rtw = kRtW;
        v[0] = (float)Reg(0x2206); v[1] = (float)rtw; v[2] = (float)kEdramH; v[3] = 0;""")
rep("""    bool useEdram = g_frameDraws > 0 && !getenv("GH6_SHOWFRONTBUFFER");""", """    static bool showEdram = getenv("GH6_SHOWEDRAM") != nullptr;
    bool useEdram = showEdram && g_frameDraws > 0;""")
rep("""omitidos prim=%llu shader=%llu pso=%llu mem=%llu, shaders %zu, psos %zu",""", """omitidos prim=%llu shader=%llu pso=%llu mem=%llu, shaders %zu, psos %zu, resolves %llu (frontbuffer %llu), superficies %zu",""")
rep("""        (unsigned long long)g_skipped[2], (unsigned long long)g_skipped[3], g_shaders.size(), g_psos.size());""", """        (unsigned long long)g_skipped[2], (unsigned long long)g_skipped[3], g_shaders.size(), g_psos.size(), (unsigned long long)g_resolves, (unsigned long long)g_fbResolves, g_edramRts.size());""")
rep("""    auto t1 = Transition(g_edram.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); l->ResourceBarrier(1, &t1);""", """    ID3D12Resource* srcRes = g_edram.Get();
    for (auto& kv : g_edramRts) if (kv.second.w == kRtW) { srcRes = kv.second.res.Get(); break; }
    auto t1 = Transition(srcRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); l->ResourceBarrier(1, &t1);""")
rep("""    D3D12_TEXTURE_COPY_LOCATION src{ g_edram.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto t2 = Transition(g_edram.Get(),""", """    D3D12_TEXTURE_COPY_LOCATION src{ srcRes, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
    D3D12_BOX dbox{ 0, 0, 0, kRtW, kRtH, 1 };
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, &dbox);
    auto t2 = Transition(srcRes,""")
open(p, "w", encoding="utf-8").write(s)

p = "G:/Guitar Hero Warrior of rock/Recomp/runtime/gpu_window.cpp"
s = open(p, encoding="utf-8").read()
rep("""            uint32_t v = *(const uint32_t*)(src + TiledInner(x, y, 2, outer)); // bytes en memoria: A R G B (BE)
            v = __builtin_bswap32(v);                                         // -> 0xAARRGGBB
            g_pixels[size_t(y) * w + x] = v | 0xFF000000u;""", """            uint32_t v = *(const uint32_t*)(src + TiledInner(x, y, 2, outer)); // k_8_8_8_8 con swap R/B: bytes B G R A
            g_pixels[size_t(y) * w + x] = v | 0xFF000000u;""")
open(p, "w", encoding="utf-8").write(s)
print("parche aplicado")
