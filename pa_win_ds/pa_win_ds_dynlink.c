#include "pa_win_ds_dynlink.h"


PaWinDsDSoundEntryPoints paWinDsDSoundEntryPoints = { 0, 0, 0, 0, 0, 0, 0 };


static HRESULT WINAPI DummyDirectSoundCreate(LPGUID lpcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
{
    (void)lpcGuidDevice; /* unused parameter */
    (void)ppDS; /* unused parameter */
    (void)pUnkOuter; /* unused parameter */
    return E_NOTIMPL;
}

static HRESULT WINAPI DummyDirectSoundEnumerateW(LPDSENUMCALLBACKW lpDSEnumCallback, LPVOID lpContext)
{
    (void)lpDSEnumCallback; /* unused parameter */
    (void)lpContext; /* unused parameter */
    return E_NOTIMPL;
}

static HRESULT WINAPI DummyDirectSoundEnumerateA(LPDSENUMCALLBACKA lpDSEnumCallback, LPVOID lpContext)
{
    (void)lpDSEnumCallback; /* unused parameter */
    (void)lpContext; /* unused parameter */
    return E_NOTIMPL;
}

static HRESULT WINAPI DummyDirectSoundCaptureCreate(LPGUID lpcGUID, LPDIRECTSOUNDCAPTURE *lplpDSC, LPUNKNOWN pUnkOuter)
{
    (void)lpcGUID; /* unused parameter */
    (void)lplpDSC; /* unused parameter */
    (void)pUnkOuter; /* unused parameter */
    return E_NOTIMPL;
}

static HRESULT WINAPI DummyDirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW lpDSCEnumCallback, LPVOID lpContext)
{
    (void)lpDSCEnumCallback; /* unused parameter */
    (void)lpContext; /* unused parameter */
    return E_NOTIMPL;
}

static HRESULT WINAPI DummyDirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA lpDSCEnumCallback, LPVOID lpContext)
{
    (void)lpDSCEnumCallback; /* unused parameter */
    (void)lpContext; /* unused parameter */
    return E_NOTIMPL;
}


void PaWinDs_InitializeDSoundEntryPoints(void)
{
    paWinDsDSoundEntryPoints.hInstance_ = LoadLibrary("dsound.dll");
    if( paWinDsDSoundEntryPoints.hInstance_ != NULL )
    {
        paWinDsDSoundEntryPoints.DirectSoundCreate =
                (HRESULT (WINAPI *)(LPGUID, LPDIRECTSOUND *, LPUNKNOWN))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundCreate" );
        if( paWinDsDSoundEntryPoints.DirectSoundCreate == NULL )
            paWinDsDSoundEntryPoints.DirectSoundCreate = DummyDirectSoundCreate;

        paWinDsDSoundEntryPoints.DirectSoundEnumerateW =
                (HRESULT (WINAPI *)(LPDSENUMCALLBACKW, LPVOID))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundEnumerateW" );
        if( paWinDsDSoundEntryPoints.DirectSoundEnumerateW == NULL )
            paWinDsDSoundEntryPoints.DirectSoundEnumerateW = DummyDirectSoundEnumerateW;

        paWinDsDSoundEntryPoints.DirectSoundEnumerateA =
                (HRESULT (WINAPI *)(LPDSENUMCALLBACKA, LPVOID))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundEnumerateA" );
        if( paWinDsDSoundEntryPoints.DirectSoundEnumerateA == NULL )
            paWinDsDSoundEntryPoints.DirectSoundEnumerateA = DummyDirectSoundEnumerateA;

        paWinDsDSoundEntryPoints.DirectSoundCaptureCreate =
                (HRESULT (WINAPI *)(LPGUID, LPDIRECTSOUNDCAPTURE *, LPUNKNOWN))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundCaptureCreate" );
        if( paWinDsDSoundEntryPoints.DirectSoundCaptureCreate == NULL )
            paWinDsDSoundEntryPoints.DirectSoundCaptureCreate = DummyDirectSoundCaptureCreate;

        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateW =
                (HRESULT (WINAPI *)(LPDSENUMCALLBACKW, LPVOID))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundCaptureEnumerateW" );
        if( paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateW == NULL )
            paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateW = DummyDirectSoundCaptureEnumerateW;

        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateA =
                (HRESULT (WINAPI *)(LPDSENUMCALLBACKA, LPVOID))
                GetProcAddress( paWinDsDSoundEntryPoints.hInstance_, "DirectSoundCaptureEnumerateA" );
        if( paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateA == NULL )
            paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateA = DummyDirectSoundCaptureEnumerateA;
    }
    else
    {
        /* initialize with dummy entry points to make live easy when ds isn't present */
        paWinDsDSoundEntryPoints.DirectSoundCreate = DummyDirectSoundCreate;
        paWinDsDSoundEntryPoints.DirectSoundEnumerateW = DummyDirectSoundEnumerateW;
        paWinDsDSoundEntryPoints.DirectSoundEnumerateA = DummyDirectSoundEnumerateA;
        paWinDsDSoundEntryPoints.DirectSoundCaptureCreate = DummyDirectSoundCaptureCreate;
        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateW = DummyDirectSoundCaptureEnumerateW;
        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateA = DummyDirectSoundCaptureEnumerateA;
    }
}


void PaWinDs_TerminateDSoundEntryPoints(void)
{
    if( paWinDsDSoundEntryPoints.hInstance_ != NULL )
    {
        /* ensure that we crash reliably if the entry points arent initialised */
        paWinDsDSoundEntryPoints.DirectSoundCreate = 0;
        paWinDsDSoundEntryPoints.DirectSoundEnumerateW = 0;
        paWinDsDSoundEntryPoints.DirectSoundEnumerateA = 0;
        paWinDsDSoundEntryPoints.DirectSoundCaptureCreate = 0;
        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateW = 0;
        paWinDsDSoundEntryPoints.DirectSoundCaptureEnumerateA = 0;

        FreeLibrary( paWinDsDSoundEntryPoints.hInstance_ );
        paWinDsDSoundEntryPoints.hInstance_ = NULL;
    }
}