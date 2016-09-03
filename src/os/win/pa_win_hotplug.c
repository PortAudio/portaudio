
#include "pa_util.h"
#include "pa_debugprint.h"
#include "pa_allocation.h"

#include "pa_win_wdmks_utils.h"

#include <windows.h>
#include <dbt.h>
#include <process.h>
#include <assert.h>
#include <ks.h>
#include <ksmedia.h>

#include <setupapi.h>

#include <stdio.h>

#if (defined(WIN32) && (defined(_MSC_VER) && (_MSC_VER >= 1200))) /* MSC version 6 and above */
#pragma comment( lib, "setupapi.lib" )
#endif


/* Implemented in pa_front.c 
  @param first  0 = unknown, 1 = insertion, 2 = removal
  @param second Host specific device change info (in windows it is the (unicode) device path)
*/
extern void PaUtil_DevicesChanged(unsigned, void*);

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define PA_THREAD_FUNC static unsigned WINAPI
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define PA_THREAD_FUNC static DWORD WINAPI
#endif

typedef struct PaHotPlugDeviceInfo
{
    wchar_t                     name[MAX_PATH];
    struct PaHotPlugDeviceInfo* next;
} PaHotPlugDeviceInfo;

typedef struct PaHotPlugDeviceEventHandlerInfo
{
    HANDLE  hWnd;
    HANDLE  hMsgThread;
    HANDLE  hNotify;
    CRITICAL_SECTION lock;
    PaUtilAllocationGroup* cacheAllocGroup;
    PaHotPlugDeviceInfo* cache;

} PaHotPlugDeviceEventHandlerInfo;


static BOOL RemoveDeviceFromCache(PaHotPlugDeviceEventHandlerInfo* pInfo, const wchar_t* name)
{
    if (pInfo->cache != NULL)
    {
        PaHotPlugDeviceInfo* lastEntry = 0;
        PaHotPlugDeviceInfo* entry = pInfo->cache;
        while (entry != NULL)
        {
            if (_wcsicmp(entry->name, name) == 0)
            {
                if (lastEntry)
                {
                    lastEntry->next = entry->next;
                }
                else
                {
                    pInfo->cache = NULL;
                }
                PaUtil_GroupFreeMemory(pInfo->cacheAllocGroup, entry);
                return TRUE;
            }

            lastEntry = entry;
            entry = entry->next;
        }
    }
    return FALSE;
}

static void InsertDeviceIntoCache(PaHotPlugDeviceEventHandlerInfo* pInfo, const wchar_t* name)
{
    PaHotPlugDeviceInfo** ppEntry = NULL;

    /* Remove it first (if possible) so we don't accidentally get duplicates */
    RemoveDeviceFromCache(pInfo, name);

    if (pInfo->cache == NULL)
    {
        ppEntry = &pInfo->cache;
    }
    else
    {
        PaHotPlugDeviceInfo* entry = pInfo->cache;
        while (entry->next != NULL)
        {
            entry = entry->next;
        }
        ppEntry = &entry->next;
    }

    *ppEntry = (PaHotPlugDeviceInfo*)PaUtil_GroupAllocateMemory(pInfo->cacheAllocGroup, sizeof(PaHotPlugDeviceInfo));
    wcsncpy((*ppEntry)->name, name, MAX_PATH-1);
    (*ppEntry)->next = NULL;
}

static BOOL IsDeviceAudio(const wchar_t* deviceName)
{
    int channelCnt = 0;
    channelCnt += PaWin_WDMKS_QueryFilterMaximumChannelCount((void*)deviceName, 1);
    channelCnt += PaWin_WDMKS_QueryFilterMaximumChannelCount((void*)deviceName, 0);
    return (channelCnt > 0);
}

static void PopulateCacheWithAvailableAudioDevices(PaHotPlugDeviceEventHandlerInfo* pInfo)
{
    HDEVINFO handle = NULL;
    const int sizeInterface = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + (MAX_PATH * sizeof(WCHAR));
    SP_DEVICE_INTERFACE_DETAIL_DATA_W* devInterfaceDetails = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)PaUtil_AllocateMemory(sizeInterface);

    if (devInterfaceDetails)
    {
        devInterfaceDetails->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        /* Open a handle to search for devices (filters) */
        handle = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO,NULL,NULL,DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if( handle != NULL )
        {
            int device;

            /* Iterate through the devices */
            for( device = 0;;device++ )
            {
                SP_DEVICE_INTERFACE_DATA interfaceData;
                SP_DEVINFO_DATA devInfoData;
                int noError;

                interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
                interfaceData.Reserved = 0;
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                devInfoData.Reserved = 0;

                noError = SetupDiEnumDeviceInterfaces(handle,NULL,&KSCATEGORY_AUDIO,device,&interfaceData);
                if( !noError )
                    break; /* No more devices */

                noError = SetupDiGetDeviceInterfaceDetailW(handle,&interfaceData,devInterfaceDetails,sizeInterface,NULL,&devInfoData);
                if( noError )
                {
                    if (IsDeviceAudio(devInterfaceDetails->DevicePath))
                    {
                        PA_DEBUG(("Hotplug cache populated with: '%S'\n", devInterfaceDetails->DevicePath));
                        InsertDeviceIntoCache(pInfo, devInterfaceDetails->DevicePath);
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(handle);
        }
        PaUtil_FreeMemory(devInterfaceDetails);
    }
}

static LRESULT CALLBACK PaMsgWinProcW(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PaHotPlugDeviceEventHandlerInfo* pInfo = (PaHotPlugDeviceEventHandlerInfo*)( GetWindowLongPtr(hWnd, GWLP_USERDATA) );
    switch(msg)
    {
    case WM_DEVICECHANGE:
        switch(wParam)
        {
        case DBT_DEVICEARRIVAL:
            {
                PDEV_BROADCAST_DEVICEINTERFACE_W ptr = (PDEV_BROADCAST_DEVICEINTERFACE_W)lParam;
                if (ptr->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                    break;

                if (!IsEqualGUID(&ptr->dbcc_classguid, &KSCATEGORY_AUDIO))
                    break;

                if (IsDeviceAudio(ptr->dbcc_name))
                {
                    PA_DEBUG(("Device inserted : %S\n", ptr->dbcc_name));
                    InsertDeviceIntoCache(pInfo, ptr->dbcc_name);
                    PaUtil_DevicesChanged(1, ptr->dbcc_name);
                }
            }
            break;
        case DBT_DEVICEREMOVECOMPLETE:
            {
                PDEV_BROADCAST_DEVICEINTERFACE_W ptr = (PDEV_BROADCAST_DEVICEINTERFACE_W)lParam;
                if (ptr->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                    break;

                if (!IsEqualGUID(&ptr->dbcc_classguid, &KSCATEGORY_AUDIO))
                    break;

                if (RemoveDeviceFromCache(pInfo, ptr->dbcc_name))
                {
                    PA_DEBUG(("Device removed  : %S\n", ptr->dbcc_name));
                    PaUtil_DevicesChanged(2, ptr->dbcc_name);
                }
            }
            break;
        default:
            break;
        }
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

PA_THREAD_FUNC PaRunMessageLoop(void* ptr)
{
    PaHotPlugDeviceEventHandlerInfo* pInfo = (PaHotPlugDeviceEventHandlerInfo*)ptr;
    WNDCLASSW wnd = { 0 };
    HMODULE hInstance = GetModuleHandleW(NULL);

    wnd.lpfnWndProc = PaMsgWinProcW;
    wnd.hInstance = hInstance;
    wnd.lpszClassName = L"{1E0D4F5A-B31F-4dcc-AE3C-4F30A47BD521}";   /* Using a GUID as class name */
    pInfo->hWnd = CreateWindowW((LPCWSTR)MAKEINTATOM(RegisterClassW(&wnd)), NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (pInfo->hWnd)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W NotificationFilter = { sizeof(DEV_BROADCAST_DEVICEINTERFACE_W) };
        NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES  0x00000004
#endif

        pInfo->hNotify = RegisterDeviceNotificationW( 
            pInfo->hWnd,                
            &NotificationFilter,        
            DEVICE_NOTIFY_WINDOW_HANDLE|DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
            );

        assert(pInfo->hNotify);

        SetWindowLongPtr(pInfo->hWnd, GWLP_USERDATA, (LONG_PTR)pInfo);

        if (pInfo->hNotify)
        {
            MSG msg; 
            BOOL result;
            while((result = GetMessageW(&msg, pInfo->hWnd, 0, 0)) != 0) 
            { 
                if (result == -1)
                {
                    break;
                }
                TranslateMessage(&msg); 
                DispatchMessageW(&msg); 
            } 
            UnregisterDeviceNotification(pInfo->hNotify);
            pInfo->hNotify = 0;
        }
        DestroyWindow(pInfo->hWnd);
        pInfo->hWnd = 0;
    }
    return 0;
}

static PaHotPlugDeviceEventHandlerInfo* s_handler = 0;

void PaUtil_InitializeHotPlug()
{
    if (s_handler == 0)
    {
        s_handler = (PaHotPlugDeviceEventHandlerInfo*)PaUtil_AllocateMemory(sizeof(PaHotPlugDeviceEventHandlerInfo));
        if (s_handler)
        {
            s_handler->cacheAllocGroup = PaUtil_CreateAllocationGroup();
            InitializeCriticalSection(&s_handler->lock);
            PopulateCacheWithAvailableAudioDevices(s_handler);
            /* Start message thread */
            s_handler->hMsgThread = CREATE_THREAD_FUNCTION(NULL, 0, PaRunMessageLoop, s_handler, 0, NULL);
            assert(s_handler->hMsgThread != 0);
        }
    }
}

void PaUtil_TerminateHotPlug()
{
    if (s_handler != 0)
    {
        if (s_handler->hWnd)
        {
            PostMessage(s_handler->hWnd, WM_QUIT, 0, 0);
            if (WaitForSingleObject(s_handler->hMsgThread, 1000) == WAIT_TIMEOUT)
            {
                TerminateThread(s_handler->hMsgThread, -1);
            }
        }
        DeleteCriticalSection(&s_handler->lock);
        PaUtil_FreeAllAllocations( s_handler->cacheAllocGroup );
        PaUtil_DestroyAllocationGroup( s_handler->cacheAllocGroup );
        PaUtil_FreeMemory( s_handler );
        s_handler = 0;
    }
}

void PaUtil_LockHotPlug()
{
    EnterCriticalSection(&s_handler->lock);
}

void PaUtil_UnlockHotPlug()
{
    LeaveCriticalSection(&s_handler->lock);
}
