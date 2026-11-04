#include "crash_dialogs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#endif

namespace chthonic {

void disableCrashDialogsIfRequested()
{
#ifdef _WIN32
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("CHTHONIC_NO_CRASH_DIALOGS", value, sizeof(value));
    if (length == 0 || length >= sizeof(value) || std::strcmp(value, "1") != 0)
        return;
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no "Debug Error!" box, no WER report
    _set_error_mode(_OUT_TO_STDERR);                               // assert() in a release build
#ifdef _DEBUG // the debug CRT's reports; release builds have none
    for (const int type : {_CRT_ERROR, _CRT_ASSERT}) { // _ASSERTE, STL debug checks, debug-CRT assert()
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // access violations: no "has stopped working" box
#endif
}

} // namespace chthonic
