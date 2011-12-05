
#include "mged3d8device.h"
#include "proxydx/d3d8texture.h"
#include "proxydx/d3d8surface.h"
#include "support/log.h"

#include "mgeversion.h"
#include "configuration.h"
#include "distantland.h"
#include "mwbridge.h"
#include "statusoverlay.h"
#include "userhud.h"

static int sceneCount = -1;
static bool rendertargetNormal = true;
static bool isHUDready = false;
static bool isMainView = false;
static bool isStencilScene = false;
static bool isFrameComplete = false;
static bool stage0Complete = false;
static bool isWaterMaterial = false;
static bool waterDrawn = false;
static RenderedState rs;

static bool detectMenu(const D3DMATRIX* m);
static void captureRenderState(D3DRENDERSTATETYPE a, DWORD b);
static void captureTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX *b);
static float calcFPS();



MGEProxyDevice::MGEProxyDevice(IDirect3DDevice9 *real, IDirect3D8 *ob) : ProxyDevice(real, ob)
{
    DECLARE_MWBRIDGE
    mwBridge->disableScreenshotFunc();

    Configuration.Zoom.zoom = 1.0;
    Configuration.Zoom.rate = 0;
    Configuration.Zoom.rateTarget = 0;

    rendertargetNormal = true;
    isHUDready = false;
}

// Present - End of MW frame
// MGE end of frame processing
HRESULT _stdcall MGEProxyDevice::Present(const RECT *a, const RECT *b, HWND c, const RGNDATA *d)
{
    DECLARE_MWBRIDGE

    // Load Morrowind's dynamic memory pointers
    if(!mwBridge->IsLoaded() && mwBridge->CanLoad())
    {
        mwBridge->Load();
        mwBridge->markWaterNode(99999.0f);
    }

    if(mwBridge->IsLoaded())
    {
        if(Configuration.Force3rdPerson && DistantLand::ready)
        {
            // Set 3rd person camera
            D3DXVECTOR3 *camera = mwBridge->PCam3Offset();
            if(camera)
            {
                camera->x = Configuration.Offset3rdPerson.x;
                camera->y = Configuration.Offset3rdPerson.y;
                camera->z = Configuration.Offset3rdPerson.z;
            }
        }

        if(Configuration.CrosshairAutohide && !mwBridge->IsLoadScreen())
        {
            // Update crosshair visibility
            static float crosshairTimeout;
            float t = mwBridge->simulationTime();

            if(mwBridge->PlayerTarget())
                crosshairTimeout = t + 1.5;

            if(mwBridge->IsPlayerCasting() || mwBridge->IsPlayerAimingWeapon())
                crosshairTimeout = t + 0.5;

            // Allow manual toggle of crosshair to work again from 0.5 seconds after timeout
            if(t < crosshairTimeout + 0.5)
                mwBridge->SetCrosshairEnabled(t < crosshairTimeout);
        }

        if(Configuration.Zoom.rateTarget != 0 && !mwBridge->IsMenu())
        {
            // Update zoom controller
            Configuration.Zoom.rate += 0.25 * Configuration.Zoom.rateTarget * mwBridge->frameTime();
            if(Configuration.Zoom.rate / Configuration.Zoom.rateTarget > 1.0)
                Configuration.Zoom.rate = Configuration.Zoom.rateTarget;

            Configuration.Zoom.zoom += Configuration.Zoom.rate * mwBridge->frameTime();
            Configuration.Zoom.zoom = std::max(1.0f, Configuration.Zoom.zoom);
            Configuration.Zoom.zoom = std::min(Configuration.Zoom.zoom, 8.0f);
        }
    }

    // Reset scene identifiers
    sceneCount = -1;
    stage0Complete = false;
    waterDrawn = false;
    isFrameComplete = false;

    return ProxyDevice::Present(a, b, c, d);
}

// SetRenderTarget
// Remember if MW is rendering to back buffer
HRESULT _stdcall MGEProxyDevice::SetRenderTarget(IDirect3DSurface8 *a, IDirect3DSurface8 *b)
{
    if(a)
    {
        IDirect3DSurface9 *back;
        realDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        rendertargetNormal = (static_cast<ProxySurface *>(a)->realSurface == back);
        back->Release();
    }

    return ProxyDevice::SetRenderTarget(a, b);
}

// BeginScene - Multiple scenes per frame, non-alpha / 2x stencil / post-stencil redraw / alpha / 1st person / UI
// Fogging needs to be set for Morrowind rendering at start of scene
HRESULT _stdcall MGEProxyDevice::BeginScene()
{
    DECLARE_MWBRIDGE

    HRESULT hr = ProxyDevice::BeginScene();
    if(hr != D3D_OK)
        return hr;

    if(mwBridge->IsLoaded() && rendertargetNormal)
    {
        if(!isHUDready)
        {
            // Initialize HUD
            StatusOverlay::init(realDevice);
            StatusOverlay::setStatus(XE_VERSION_STRING);
            MGEhud::init(realDevice);
            isHUDready = true;
        }

        if(isMainView)
        {
            // Track scene count here in BeginSene
            // isMainView is not always valid at EndScene if Morrowind draws sunglare
            ++sceneCount;

            // Set any custom FOV
            if(sceneCount == 0 && Configuration.ScreenFOV > 0)
                mwBridge->SetFOV(Configuration.ScreenFOV);

            if(!DistantLand::ready)
            {
                if(DistantLand::init(realDevice))
                    // Initially force view distance to max, required for full extent shadows
                    mwBridge->SetViewDistance(7168.0);
                else
                    StatusOverlay::setStatus("Serious error. Check mgeXE.log for details.");
            }
        }
        else
        {
            // UI scene, apply post-process if there was anything drawn before it
            // Race menu will render an extra scene past this point
            if(DistantLand::ready && sceneCount > 0 && !isFrameComplete)
                DistantLand::postProcess();

            isFrameComplete = true;
        }
    }

    return D3D_OK;
}

// EndScene - Multiple scenes per frame, non-alpha / 2x stencil / post-stencil redraw / alpha / 1st person / UI
// MGE intercepts first scene to draw distant land before it finishes, others it applies shadows to
HRESULT _stdcall MGEProxyDevice::EndScene()
{
    DECLARE_MWBRIDGE

    if(DistantLand::ready && rendertargetNormal)
    {
        // The following Morrowind scenes get past the filters:
        // ~ Opaque meshes, plus alpha meshes with 'No Sorter' property (which should use alpha test)
        // ~ If stencil shadows are active, then shadow casters are deferred to be drawn in a scene after
        //    shadows are fully applied to avoid self-shadowing problems with simplified shadow meshes
        // ~ If any alpha meshes are visible, they are sorted and drawn in another scene (except those with 'No Sorter' property)
        // ~ If 1st person or sunglare is visible, they are drawn in another scene after a Z clear
        if(sceneCount == 0)
        {
            // Edge case, render distant land even if Morrowind has culled everything
            if(!stage0Complete)
            {
                DistantLand::renderStage0();
                stage0Complete = true;
            }

            // Opaque features
            DistantLand::renderStage1();
        }
        else if(!isFrameComplete)
        {
            // Everything else except UI
            DistantLand::renderStage2();

            // Draw water in exceptional conditions
            // Water is undetectable if too distant or stencil scene order is non-normative
            if(!waterDrawn && !isStencilScene)
            {
                DistantLand::renderBlend();
                waterDrawn = true;
            }
        }
    }

    if(isFrameComplete && isHUDready)
    {
        // Render user hud
        MGEhud::draw();

        // Render status overlay
        StatusOverlay::setFPS(calcFPS());
        StatusOverlay::show(realDevice);
    }

    return ProxyDevice::EndScene();
}

// Clear - Main frame, skybox doesn't extend over whole background
// Background colour is visible at horizon
HRESULT _stdcall MGEProxyDevice::Clear(DWORD a, const D3DRECT *b, DWORD c, D3DCOLOR d, float e, DWORD f)
{
    DistantLand::setHorizonColour(d);
    return ProxyDevice::Clear(a, b, c, d, e, f);
}

// SetTransform
// Projection needs modifying to allow room for distant land
HRESULT _stdcall MGEProxyDevice::SetTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX *b)
{
    captureTransform(a, b);

    if(rendertargetNormal)
    {
        if(a == D3DTS_VIEW)
        {
            // Check for UI view
            isMainView = !detectMenu(b);
        }
        else if(a == D3DTS_PROJECTION)
        {
            // Only screw with main scene projection
            if(isMainView)
            {
                D3DXMATRIX proj = *b;
                DistantLand::setProjection(&proj);

                if(Configuration.MGEFlags & ZOOM_ASPECT)
                {
                    proj._11 *= Configuration.Zoom.zoom;
                    proj._22 *= Configuration.Zoom.zoom;
                }

                return ProxyDevice::SetTransform(a, &proj);
            }
        }
    }

    return ProxyDevice::SetTransform(a, b);
}

// SetMaterial
// Check for materials marked for hiding
HRESULT _stdcall MGEProxyDevice::SetMaterial(const D3DMATERIAL8 *a)
{
    isWaterMaterial = (a->Power == 99999.0f);
    return ProxyDevice::SetMaterial(a);
}

// SetLight
// Exterior sunlight appears to always be light 6
HRESULT _stdcall MGEProxyDevice::SetLight(DWORD a, const D3DLIGHT8 *b)
{
    if(a == 6)
        DistantLand::setSunLight(b);

    return ProxyDevice::SetLight(a, b);
}

// SetRenderState
// Ignore Morrowind fog settings, and run stage 0 rendering after lighting setup
HRESULT _stdcall MGEProxyDevice::SetRenderState(D3DRENDERSTATETYPE a, DWORD b)
{
    captureRenderState(a, b);

    if(a == D3DRS_FOGVERTEXMODE || a == D3DRS_FOGTABLEMODE)
        return D3D_OK;
    if((Configuration.MGEFlags & USE_DISTANT_LAND) && (a == D3DRS_FOGSTART || a == D3DRS_FOGEND))
        return D3D_OK;
    if(a == D3DRS_STENCILENABLE)
        isStencilScene = b;

    // Ambient is the final setting in Morrowind light setup, directly after sky rendering
    // Ignore pure white ambient, most likely to be menu mode setting
    if(a == D3DRS_AMBIENT && b != 0xffffffff)
    {
        // Ambient is also never set properly when high enough outside that Morrowind renders nothing
        // Avoid changing ambient to pure white in this case
        DistantLand::setAmbientColour(b);

        if(DistantLand::ready && !stage0Complete && sceneCount == 0)
        {
            // At this point, only the sky is rendered
            ProxyDevice::SetRenderState(a, b);
            DistantLand::renderStage0();
            stage0Complete = true;
        }
    }

    return ProxyDevice::SetRenderState(a, b);
}

// SetTextureStageState
// Override some sampler options
HRESULT _stdcall MGEProxyDevice::SetTextureStageState(DWORD a, D3DTEXTURESTAGESTATETYPE b, DWORD c)
{
    // Overrides
    if(a == 0)
    {
        if(b == D3DTSS_MINFILTER && c == 2)
            return realDevice->SetSamplerState(0, D3DSAMP_MINFILTER, Configuration.ScaleFilter);

        if(b == D3DTSS_MIPFILTER && c == 2)
            return realDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, Configuration.MipFilter);

        if(b == D3DTSS_MIPMAPLODBIAS && Configuration.LODBias)
            return realDevice->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, *(DWORD *)&Configuration.LODBias);
    }

    return ProxyDevice::SetTextureStageState(a, b, c);
}

// DrawIndexedPrimitive - Where all the drawing happens
// Inspect draw calls for re-use later
HRESULT _stdcall MGEProxyDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE a, UINT b, UINT c, UINT d, UINT e)
{
    // Allow distant land to inspect draw calls
    if(DistantLand::ready && rendertargetNormal && isMainView && !isStencilScene)
    {
        rs.primType = a; rs.baseIndex = baseVertexIndex; rs.minIndex = b; rs.vertCount = c; rs.startIndex = d; rs.primCount = e;
        if(!DistantLand::inspectIndexedPrimitive(sceneCount, &rs))
            return D3D_OK;

        // Call distant land instead of drawing water grid
        if(isWaterMaterial)
        {
            if(!waterDrawn)
            {
                DistantLand::renderBlend();
                waterDrawn = true;
            }
            return D3D_OK;
        }
    }

    return ProxyDevice::DrawIndexedPrimitive(a, b, c, d, e);
}

// Release - Free all resources when refcount hits 0
ULONG _stdcall MGEProxyDevice::Release()
{
    ULONG r = ProxyDevice::Release();

    if(r == 0)
    {
        DistantLand::release();
        MGEhud::release();
        StatusOverlay::release();
    }

    return r;
}

// --------------------------------------------------------

// detectMenu
// detects if view matrix is for UI / load bars
// the projection matrix is never set to ortho, unusable for detection
bool detectMenu(const D3DMATRIX* m)
{
    if(m->_41 != 0.0f || !(m->_42 == 0.0f || m->_42 == -600.0f) || m->_43 != 0.0f)
        return false;

    if((m->_11 == 0.0f || m->_11 == 1.0f) && m->_12 == 0.0f && (m->_13 == 0.0f || m->_13 == 1.0f) &&
        m->_21 == 0.0f && (m->_22 == 0.0f || m->_22 == 1.0f) && (m->_23 == 0.0f || m->_23 == 1.0f) &&
        (m->_31 == 0.0f || m->_31 == 1.0f) && (m->_32 == 0.0f || m->_32 == 1.0f) && m->_33 == 0.0f)
        return true;

    return false;
}

// --------------------------------------------------------
// State recording

HRESULT _stdcall MGEProxyDevice::SetTexture(DWORD a, IDirect3DBaseTexture8 *b)
{
    if(a == 0) { rs.texture = b ? static_cast<ProxyTexture*>(b)->realTexture : NULL; }
    return ProxyDevice::SetTexture(a, b);
}

HRESULT _stdcall MGEProxyDevice::SetVertexShader(DWORD a)
{
    rs.fvf = a;
    return ProxyDevice::SetVertexShader(a);
}

HRESULT _stdcall MGEProxyDevice::SetStreamSource(UINT a, IDirect3DVertexBuffer8 *b, UINT c)
{
    if(a == 0) { rs.vb = (IDirect3DVertexBuffer9*)b; rs.vbOffset = 0; rs.vbStride = c; }
    return ProxyDevice::SetStreamSource(a, b, c);
}

HRESULT _stdcall MGEProxyDevice::SetIndices(IDirect3DIndexBuffer8 *a, UINT b)
{
    rs.ib = (IDirect3DIndexBuffer9*)a;
    return ProxyDevice::SetIndices(a, b);
}

void captureRenderState(D3DRENDERSTATETYPE a, DWORD b)
{
    if(a == D3DRS_VERTEXBLEND) rs.vertexBlendState = b;
    else if(a == D3DRS_ZWRITEENABLE) rs.zWrite = b;
    else if(a == D3DRS_CULLMODE) rs.cullMode = b;
    else if(a == D3DRS_ALPHABLENDENABLE) rs.blendEnable = b;
    else if(a == D3DRS_SRCBLEND) rs.srcBlend = b;
    else if(a == D3DRS_DESTBLEND) rs.destBlend = b;
    else if(a == D3DRS_ALPHATESTENABLE) rs.alphaTest = b;
    else if(a == D3DRS_ALPHAFUNC) rs.alphaFunc = b;
    else if(a == D3DRS_ALPHAREF) rs.alphaRef = b;
}

void captureTransform(D3DTRANSFORMSTATETYPE a, const D3DMATRIX *b)
{
    if(a == D3DTS_WORLDMATRIX(0)) rs.worldTransforms[0] = *b;
    else if(a == D3DTS_WORLDMATRIX(1)) rs.worldTransforms[1] = *b;
    else if(a == D3DTS_WORLDMATRIX(2)) rs.worldTransforms[2] = *b;
    else if(a == D3DTS_WORLDMATRIX(3)) rs.worldTransforms[3] = *b;
}

float calcFPS()
{
    static DWORD lasttick, tick, f;
    static float fps;

    ++f;
    tick = GetTickCount();
    if((tick - lasttick) > 1000)
    {
        fps = 1000.0 * f / (tick - lasttick);
        lasttick = tick;
        f = 0;
    }

    return fps;
}
