#include <windows.h>

#include "app.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return appMain(hInstance, GetCommandLineW(), nCmdShow);
}
