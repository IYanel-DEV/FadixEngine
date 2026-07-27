#include <windows.h>
#include <iostream>
#include <string>

int main() {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hSharedFile = CreateFileMapping(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
    HANDLE hEventBufferReady = CreateEvent(&sa, FALSE, FALSE, "DBWIN_BUFFER_READY");
    HANDLE hEventDataReady = CreateEvent(&sa, FALSE, FALSE, "DBWIN_DATA_READY");

    if (!hSharedFile || !hEventBufferReady || !hEventDataReady) {
        std::cerr << "Failed to create debug objects." << std::endl;
        return 1;
    }

    LPVOID pBuffer = MapViewOfFile(hSharedFile, FILE_MAP_READ, 0, 0, 4096);
    if (!pBuffer) {
        std::cerr << "Failed to map view of file." << std::endl;
        return 1;
    }

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcess("bin\\Debug\\fadix_editor.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        std::cerr << "Failed to start fadix_editor.exe" << std::endl;
        return 1;
    }

    SetEvent(hEventBufferReady);

    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        if (WaitForSingleObject(hEventDataReady, 100) == WAIT_OBJECT_0) {
            DWORD processId = *((DWORD*)pBuffer);
            if (processId == pi.dwProcessId) {
                const char* msg = (const char*)pBuffer + sizeof(DWORD);
                std::cout << "[D3D12] " << msg;
            }
            SetEvent(hEventBufferReady);
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    UnmapViewOfFile(pBuffer);
    CloseHandle(hSharedFile);
    CloseHandle(hEventBufferReady);
    CloseHandle(hEventDataReady);

    return 0;
}
