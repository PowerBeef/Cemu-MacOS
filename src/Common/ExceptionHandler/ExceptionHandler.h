#pragma once

void ExceptionHandler_Init();
// sigaltstack is per-thread; every thread that could fault needs one or a
// stack-overflow SIGSEGV cannot be reported
void ExceptionHandler_RegisterAltStackForThisThread();

bool CrashLog_Create();
void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt);
void CrashLog_WriteLine(std::string_view text, bool newLine = true);
void CrashLog_WriteHeader(const char* header);

void ExceptionHandler_LogGeneralInfo();
