// Tabla de handles del "kernel" y objetos (eventos, hilos, archivos...).
#pragma once
#include "runtime.h"
#include <windows.h>
#include <memory>
#include <string>

// Tipos de cabecera de dispatcher del kernel de Xbox 360 (byte 0 de X_DISPATCHER_HEADER)
enum : uint8_t { kObjNotificationEvent = 0, kObjSyncEvent = 1, kObjMutant = 2, kObjSemaphore = 5, kObjThread = 6, kObjTimerNotif = 8, kObjTimerSync = 9 };

struct KObject
{
    virtual ~KObject() { if (waitable) CloseHandle(waitable); }
    HANDLE waitable = nullptr; // objeto Win32 equivalente (si se puede esperar)
    uint8_t type = 0xFF;
    uint32_t guestPtr = 0;     // objeto en memoria de invitado (ObReferenceObjectByHandle / Ke*)
};

constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
constexpr uint32_t XSTATUS_SUCCESS = 0, XSTATUS_TIMEOUT = 0x102, XSTATUS_PENDING = 0x103, XSTATUS_INVALID_HANDLE = 0xC0000008,
    XSTATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, XSTATUS_END_OF_FILE = 0xC0000011, XSTATUS_NO_MORE_FILES = 0x80000006,
    XSTATUS_NO_SUCH_FILE = 0xC000000F, XSTATUS_ACCESS_DENIED = 0xC0000022, XSTATUS_INVALID_PARAMETER = 0xC000000D,
    XSTATUS_NOT_IMPLEMENTED = 0xC0000002, XSTATUS_OBJECT_NAME_COLLISION = 0xC0000035, XSTATUS_BUFFER_TOO_SMALL = 0xC0000023;

uint32_t InsertHandle(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> GetHandleObject(uint32_t handle);
bool CloseGuestHandle(uint32_t handle);
// Objeto de kernel a partir de un puntero de invitado (lo crea a partir de la cabecera si no existe)
std::shared_ptr<KObject> GetObjectByPointer(uint32_t ptr);
uint32_t EnsureGuestPointer(const std::shared_ptr<KObject>& obj);
// Timeout de invitado (puntero a int64 BE en unidades de 100 ns; negativo = relativo) -> ms
DWORD GuestTimeoutToMs(uint32_t timeoutPtr);
uint32_t WaitResult(DWORD r, uint32_t count = 1);
