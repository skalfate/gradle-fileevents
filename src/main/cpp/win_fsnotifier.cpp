#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN7
//#define _WIN32_WINNT  _WIN32_WINNT_WIN7
#endif

#ifdef _WIN32

#include "win_fsnotifier.h"
#include "command.h"

#include <locale>

// For Windows 7 SDK, this Windows 10 symbol is missing and must be defined to compile correctly
#if (NTDDI_VERSION == NTDDI_WIN7)

enum READ_DIRECTORY_NOTIFY_INFORMATION_CLASS {
    ReadDirectoryNotifyInformation = 1,
    ReadDirectoryNotifyExtendedInformation = 2
};

// This function pointer uses native Windows type name for Windows 10 Support
typedef BOOL(WINAPI* PFN_ReadDirectoryChangesExW)(
    HANDLE, LPVOID, DWORD, BOOL, DWORD, LPDWORD, LPOVERLAPPED,
    LPOVERLAPPED_COMPLETION_ROUTINE, READ_DIRECTORY_NOTIFY_INFORMATION_CLASS
);
#endif

#ifndef FILE_NOTIFY_CHANGE_LAST_ACCESS

typedef struct _FILE_NOTIFY_EXTENDED_INFORMATION {
    ULONG NextEntryOffset;
    ULONG Action;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastModificationTime;
    LARGE_INTEGER LastChangeTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER AllocatedLength;
    LARGE_INTEGER FileSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_NOTIFY_EXTENDED_INFORMATION, *PFILE_NOTIFY_EXTENDED_INFORMATION;
#endif

using namespace std;

string wideToUtf8String(const wstring& wstr) {
    if (wstr.empty()) return std::string();

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Size == 0) {
        return std::string("INVALID_UTF8");
    }

    std::string utf8Str(utf8Size, '\0');

    int result = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &utf8Str[0], utf8Size, nullptr, nullptr);
    if (result == 0) {
        return std::string("INVALID_UTF8");
    }

    return utf8Str;
}

#define wideToUtf16String(string) (u16string((string).begin(), (string).end()))

bool isAbsoluteLocalPath(const wstring& path) {
    if (path.length() < 3) {
        return false;
    }
    return ((L'a' <= path[0] && path[0] <= L'z') || (L'A' <= path[0] && path[0] <= L'Z'))
        && path[1] == L':'
        && path[2] == L'\\';
}

bool isAbsoluteUncPath(const wstring& path) {
    if (path.length() < 3) {
        return false;
    }
    return path[0] == L'\\' && path[1] == L'\\';
}

bool isLongPath(const wstring& path) {
    return path.length() >= 4 && path.substr(0, 4) == L"\\\\?\\";
}

bool isUncLongPath(const wstring& path) {
    return path.length() >= 8 && path.substr(0, 8) == L"\\\\?\\UNC\\";
}

// TODO How can this be done nicer, without both unnecessary copy and in-place mutation?
void convertToLongPathIfNeeded(wstring& path) {
    // Technically, this should be MAX_PATH (i.e. 260), except some Win32 API related
    // to working with directory paths are actually limited to 240. It is just
    // safer/simpler to cover both cases in one code path.
    if (path.length() <= 240) {
        return;
    }

    // It is already a long path, nothing to do here
    if (isLongPath(path)) {
        return;
    }

    if (isAbsoluteLocalPath(path)) {
        // Format: C:\... -> \\?\C:\...
        path.insert(0, L"\\\\?\\");
    } else if (isAbsoluteUncPath(path)) {
        // In this case, we need to skip the first 2 characters:
        // Format: \\server\share\... -> \\?\UNC\server\share\...
        path.erase(0, 2);
        path.insert(0, L"\\\\?\\UNC\\");
    } else {
        // It is some sort of unknown format, don't mess with it
    }
}

// Allocate maximum path length
#define PATH_BUFFER_SIZE 32768

bool resolveFinalPath(HANDLE handle, wstring& path) {
    vector<wchar_t> buffer;
    buffer.reserve(PATH_BUFFER_SIZE);
    DWORD pathLength = GetFinalPathNameByHandleW(
        handle,
        &buffer[0],
        PATH_BUFFER_SIZE,
        FILE_NAME_OPENED);
    if (pathLength == 0 || pathLength > PATH_BUFFER_SIZE) {
        logToJava(LogLevel::WARN_LEVEL, "Couldn't get final path for handle 0x%x, error code: %d", handle, GetLastError());
        return false;
    }
    path.clear();
    path.insert(0, &buffer[0], pathLength);
    return true;
}

//
// WatchPoint
//

WatchPoint::WatchPoint(Server* server, size_t eventBufferSize, const wstring& path)
    : server(server)
    , registeredPath(path)
    , status(WatchPointStatus::NOT_LISTENING) {
    wstring longPath = path;
    convertToLongPathIfNeeded(longPath);
    HANDLE directoryHandle = CreateFileW(
        longPath.c_str(),       // pointer to the file name
        FILE_LIST_DIRECTORY,    // access (read/write) mode
        CREATE_SHARE,           // share mode
        NULL,                   // security descriptor
        OPEN_EXISTING,          // how to create
        CREATE_FLAGS,           // file attributes
        NULL                    // file with attributes to copy
    );
    if (directoryHandle == INVALID_HANDLE_VALUE) {
        throw FileWatcherException("Couldn't add watch", wideToUtf16String(path), GetLastError());
    }
    this->directoryHandle = directoryHandle;
    bool directoryHandleIsAccessible = resolveFinalPath(directoryHandle, registeredFinalPath);
    if (!directoryHandleIsAccessible) {
        throw FileWatcherException("Couldn't resolve final path of", wideToUtf16String(path), GetLastError());
    }
    this->eventBuffer.reserve(eventBufferSize);
    ZeroMemory(&this->overlapped, sizeof(OVERLAPPED));
    this->overlapped.hEvent = this;
    switch (listen()) {
        case ListenResult::SUCCESS:
            break;
        case ListenResult::DELETED:
            throw FileWatcherException("Couldn't add watch, path is not a directory", wideToUtf16String(path));
    }
}

bool WatchPoint::cancel() {
    if (status == WatchPointStatus::LISTENING) {
        logToJava(LogLevel::TRACE_LEVEL, "Cancelling %s", wideToUtf8String(registeredPath).c_str());
        bool cancelled = (bool) CancelIoEx(directoryHandle, &overlapped);
        if (cancelled) {
            status = WatchPointStatus::CANCELLED;
        } else {
            DWORD cancelError = GetLastError();
            close();
            if (cancelError == ERROR_NOT_FOUND) {
                // Do nothing, looks like this is a typical scenario
                logToJava(LogLevel::TRACE_LEVEL, "Watch point already finished %s", wideToUtf8String(registeredPath).c_str());
            } else {
                throw FileWatcherException("Couldn't cancel watch point", wideToUtf16String(registeredPath), cancelError);
            }
        }
        return cancelled;
    }
    return false;
}

WatchPoint::~WatchPoint() {
    try {
        cancel();
        SleepEx(0, true);
        close();
    } catch (const exception& ex) {
        logToJava(LogLevel::WARN_LEVEL, "Couldn't cancel watch point %s: %s", wideToUtf8String(registeredPath).c_str(), ex.what());
    }
}

static void CALLBACK handleEventCallback(DWORD errorCode, DWORD bytesTransferred, LPOVERLAPPED overlapped) {
    WatchPoint* watchPoint = (WatchPoint*) overlapped->hEvent;
    watchPoint->handleEventsInBuffer(errorCode, bytesTransferred);
}

bool WatchPoint::isValidDirectory() {
    DWORD attrib = GetFileAttributesW(registeredPath.c_str());

    return (attrib != INVALID_FILE_ATTRIBUTES)
        && ((attrib & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

ListenResult WatchPoint::listen() {
    // Look up the Windows 10 API entry point at runtime
    static PFN_ReadDirectoryChangesExW pfnReadDirectoryChangesExW =
        (PFN_ReadDirectoryChangesExW)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadDirectoryChangesExW");

    BOOL success = FALSE;

    if (pfnReadDirectoryChangesExW != nullptr) {
        // WINDOWS 10 API PATH
        success = pfnReadDirectoryChangesExW(
            directoryHandle,                   // handle to directory
            &eventBuffer[0],                   // read results buffer
            (DWORD) eventBuffer.capacity(),    // length of buffer
            TRUE,                              // include children
            EVENT_MASK,                        // filter conditions
            NULL,                              // bytes returned
            &overlapped,                       // overlapped buffer
            &handleEventCallback,              // completion routine
            ReadDirectoryNotifyExtendedInformation // Request the modern layout
        );
    } else {
        // WINDOWS 7 API BACKWARD-COMPATIBLE FALLBACK
        success = ReadDirectoryChangesW(
            directoryHandle,                   // handle to directory
            &eventBuffer[0],                   // read results buffer
            (DWORD) eventBuffer.capacity(),    // length of buffer
            TRUE,                              // include children
            EVENT_MASK,                        // filter conditions
            NULL,                              // bytes returned
            &overlapped,                       // overlapped buffer
            &handleEventCallback              // completion routine
        );
    }

    if (success) {
        status = WatchPointStatus::LISTENING;
        return ListenResult::SUCCESS;
    } else {
        DWORD listenError = GetLastError();
        close();
        if (listenError == ERROR_ACCESS_DENIED && !isValidDirectory()) {
            return ListenResult::DELETED;
        } else {
            throw FileWatcherException("Couldn't add watch", wideToUtf16String(registeredPath), listenError);
        }
    }
}

void WatchPoint::close() {
    if (status != WatchPointStatus::FINISHED) {
        try {
            BOOL ret = CloseHandle(directoryHandle);
            if (!ret) {
                logToJava(LogLevel::ERROR_LEVEL, "Couldn't close handle %p for '%ls': %d", directoryHandle, wideToUtf8String(registeredPath).c_str(), GetLastError());
            }
        } catch (const exception& ex) {
            // Apparently with debugging enabled CloseHandle() can also throw, see:
            // https://docs.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle#return-value
            logToJava(LogLevel::ERROR_LEVEL, "Couldn't close handle %p for '%ls': %s", directoryHandle, wideToUtf8String(registeredPath).c_str(), ex.what());
        }
        status = WatchPointStatus::FINISHED;
    }
}

void WatchPoint::handleEventsInBuffer(DWORD errorCode, DWORD bytesTransferred) {
    if (errorCode == ERROR_OPERATION_ABORTED) {
        logToJava(LogLevel::TRACE_LEVEL, "Finished watching '%s', status = %d", wideToUtf8String(registeredPath).c_str(), status);
        close();
        return;
    }

    if (status != WatchPointStatus::LISTENING) {
        logToJava(LogLevel::TRACE_LEVEL, "Ignoring incoming events for %s as watch-point is not listening (%d bytes, errorCode = %d, status = %d)",
            wideToUtf8String(registeredPath).c_str(), bytesTransferred, errorCode, status);
        return;
    }
    status = WatchPointStatus::NOT_LISTENING;
    server->handleEvents(this, errorCode, eventBuffer, bytesTransferred);
}

//
// Server
//

void Server::handleEvents(WatchPoint* watchPoint, DWORD errorCode, const vector<BYTE>& eventBuffer, DWORD bytesTransferred) {
    JNIEnv* env = getThreadEnv();

    try {
        if (errorCode != ERROR_SUCCESS) {
            if (errorCode == ERROR_ACCESS_DENIED && !watchPoint->isValidDirectory()) {
                reportWatchPointDeleted(watchPoint);
                return;
            } else {
                throw FileWatcherException("Error received when handling events", wideToUtf16String(watchPoint->registeredPath), errorCode);
            }
        }

        wstring currentFinalPath;
        bool watchedHandleIsAccessible = resolveFinalPath(watchPoint->directoryHandle, currentFinalPath);
        if (!watchedHandleIsAccessible || currentFinalPath != watchPoint->registeredFinalPath) {
            // The handle has become invalid or missing, or the directory has been relocated, consider this as if the the watch point was deleted
            reportWatchPointDeleted(watchPoint);
            return;
        }

        const wstring& path = watchPoint->registeredPath;
        if (shouldTerminate) {
            logToJava(LogLevel::TRACE_LEVEL, "Ignoring incoming events for %s because server is terminating (%d bytes, status = %d)",
                wideToUtf8String(path).c_str(), bytesTransferred, watchPoint->status);
            return;
        }

        if (bytesTransferred == 0) {
            // This is what the documentation has to say about a zero-length dataset:
            //
            //     If the number of bytes transferred is zero, the eventBuffer was either too large
            //     for the system to allocate or too small to provide detailed information on
            //     all the changes that occurred in the directory or subtree. In this case,
            //     you should compute the changes by enumerating the directory or subtree.
            //
            // (See https://docs.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
            //
            // We'll handle this as a simple overflow and report it as such.
            reportOverflow(env, wideToUtf16String(path));
        } else {
            // Re-check operating system support scope to parse accurately
            static bool isWindows10_OrNewer = (GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadDirectoryChangesExW") != nullptr);
            int index = 0;

            for (;;) {
                if (isWindows10_OrNewer) {
                    // Parse using Windows 10 Payload Standards
                    FILE_NOTIFY_EXTENDED_INFORMATION* currentEx = (FILE_NOTIFY_EXTENDED_INFORMATION*)&eventBuffer[index];

                    // Construct local variables to pass to common processing routine
                    wstring changedPathW = wstring(currentEx->FileName, currentEx->FileNameLength / sizeof(wchar_t));
                    bool isDirectory = (currentEx->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                    handleCommonEvent(env, path, currentEx->Action, changedPathW, isDirectory, true);

                    if (currentEx->NextEntryOffset == 0) {break;};
                    index += currentEx->NextEntryOffset;
                } else {
                    // Parse using Windows 7 Payload Standards
                    FILE_NOTIFY_INFORMATION* current7 = (FILE_NOTIFY_INFORMATION*)&eventBuffer[index];

                    wstring changedPathW = wstring(current7->FileName, current7->FileNameLength / sizeof(wchar_t));

                    // Windows 7 payload doesn't contain attribute metadata
                    handleCommonEvent(env, path, current7->Action, changedPathW, false, false);

                    if (current7->NextEntryOffset == 0) {break;};
                    index += current7->NextEntryOffset;
                }
            }
        }

        switch (watchPoint->listen()) {
            case ListenResult::SUCCESS:
                break;
            case ListenResult::DELETED:
                logToJava(LogLevel::TRACE_LEVEL, "Watched directory removed for %s", wideToUtf8String(path).c_str());
                reportChangeEvent(env, ChangeType::REMOVED, wideToUtf16String(path));
                break;
        }
    } catch (const exception& ex) {
        reportFailure(env, ex);
    }
}

// Universal Handle Event for Windows 7 and 10
void Server::handleCommonEvent(JNIEnv* env, const wstring& watchedPathW, DWORD action, const wstring& relativePathW, bool isDirectory, bool hasAttributes) {
    wstring changedPathW = relativePathW;
    if (!changedPathW.empty()) {
        changedPathW.insert(0, 1, L'\\');
    }
    changedPathW.insert(0, watchedPathW);

    logToJava(LogLevel::TRACE_LEVEL, "Change detected: 0x%x '%s'", action, wideToUtf8String(changedPathW).c_str());

    ChangeType type;
    if (action == FILE_ACTION_ADDED || action == FILE_ACTION_RENAMED_NEW_NAME) {
        type = ChangeType::CREATED;
    } else if (action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME) {
        type = ChangeType::REMOVED;
    } else if (action == FILE_ACTION_MODIFIED) {
        // Apply optimization only if attributes are safely exposed (Windows 10 path)
        if (hasAttributes && isDirectory) {
             // Ignore MODIFIED events on directories
            logToJava(LogLevel::TRACE_LEVEL, "Ignored MODIFIED event on directory", nullptr);
            return;
        }
        type = ChangeType::MODIFIED;
    } else {
        logToJava(LogLevel::WARN_LEVEL, "Unknown event 0x%x for %s", action, wideToUtf8String(changedPathW).c_str());
        reportUnknownEvent(env, wideToUtf16String(changedPathW));
        return;
    }

    reportChangeEvent(env, type, wideToUtf16String(changedPathW));
}

void Server::reportWatchPointDeleted(WatchPoint* watchPoint) {
    reportChangeEvent(getThreadEnv(), ChangeType::REMOVED, wideToUtf16String(watchPoint->registeredPath));
    watchPoint->close();
}

Server::Server(JNIEnv* env, size_t eventBufferSize, long commandTimeoutInMillis, jobject watcherCallback)
    : AbstractServer(env, watcherCallback)
    , eventBufferSize(eventBufferSize)
    , commandTimeoutInMillis(commandTimeoutInMillis) {
    jclass listClass = env->FindClass("java/util/List");
    this->listAddMethod = env->GetMethodID(listClass, "add", "(Ljava/lang/Object;)Z");
}

void Server::initializeRunLoop() {
    // For some reason GetCurrentThread() returns a thread that doesn't accept APCs
    // so we need to use OpenThread() instead.
    threadHandle = OpenThread(
        THREAD_ALL_ACCESS,      // dwDesiredAccess
        false,                  // bInheritHandle
        GetCurrentThreadId()    // dwThreadId
    );
    if (threadHandle == NULL) {
        throw FileWatcherException("Couldn't open current thread", GetLastError());
    }
}

void Server::shutdownRunLoop() {
    executeOnRunLoop([this]() {
        shouldTerminate = true;
        return true;
    });
}

void Server::runLoop() {
    while (!shouldTerminate) {
        SleepEx(INFINITE, true);
    }

    // We have received termination, cancel all watchers
    logToJava(LogLevel::TRACE_LEVEL, "Finished with run loop, now cancelling remaining watch points", NULL);
    for (auto& it : watchPoints) {
        auto& watchPoint = it.second;
        if (watchPoint.status == WatchPointStatus::LISTENING) {
            try {
                watchPoint.cancel();
            } catch (const exception& ex) {
                logToJava(LogLevel::ERROR_LEVEL, "%s", ex.what());
            }
        }
    }

    logToJava(LogLevel::TRACE_LEVEL, "Waiting for any pending watch points to abort completely", NULL);
    SleepEx(0, true);

    // Warn about  any unfinished watchpoints
    for (auto& it : watchPoints) {
        auto& watchPoint = it.second;
        switch (watchPoint.status) {
            case WatchPointStatus::NOT_LISTENING:
            case WatchPointStatus::FINISHED:
                break;
            default:
                logToJava(LogLevel::WARN_LEVEL, "Watch point %s did not finish before termination timeout (status = %d)",
                    wideToUtf8String(watchPoint.registeredPath).c_str(), watchPoint.status);
                break;
        }
    }

    CloseHandle(threadHandle);
}

static void CALLBACK executeOnRunLoopCallback(_In_ ULONG_PTR info) {
    Command* command = (Command*) info;
    command->executeInsideRunLoop();
}

bool Server::executeOnRunLoop(function<bool()> function) {
    Command command(function);
    return command.execute(commandTimeoutInMillis, [this](Command* command) {
        DWORD ret = QueueUserAPC(executeOnRunLoopCallback, threadHandle, (ULONG_PTR) command);
        if (ret == 0) {
            throw FileWatcherException("Received error while queuing APC", GetLastError());
        }
    });
}

void Server::registerPaths(const vector<u16string>& paths) {
    executeOnRunLoop([this, paths]() {
        for (auto& path : paths) {
            registerPath(path);
        }
        return true;
    });
}

bool Server::unregisterPaths(const vector<u16string>& paths) {
    return executeOnRunLoop([this, paths]() {
        bool success = true;
        for (auto& path : paths) {
            success &= unregisterPath(path);
        }
        return success;
    });
}

void Server::registerPath(const u16string& path) {
    wstring registeredPath(path.begin(), path.end());
    auto it = watchPoints.find(registeredPath);
    if (it != watchPoints.end()) {
        if (it->second.status == WatchPointStatus::FINISHED) {
            watchPoints.erase(it);
        } else {
            throw FileWatcherException("Already watching path", path);
        }
    }
    watchPoints.emplace(piecewise_construct,
        forward_as_tuple(registeredPath),
        forward_as_tuple(this, eventBufferSize, registeredPath));
}

bool Server::unregisterPath(const u16string& path) {
    wstring registeredPath(path.begin(), path.end());
    if (watchPoints.erase(registeredPath) == 0) {
        logToJava(LogLevel::INFO_LEVEL, "Path is not watched: %s", wideToUtf8String(registeredPath).c_str());
        return false;
    }
    return true;
}

void Server::stopWatchingMovedPaths(jobject droppedPaths) {
    JNIEnv* env = getThreadEnv();
    for (auto& it : watchPoints) {
        auto& watchPoint = it.second;
        if (watchPoint.status == WatchPointStatus::FINISHED) {
            continue;
        }
        wstring currentFinalPath;
        bool watchedHandleIsAccessible = resolveFinalPath(watchPoint.directoryHandle, currentFinalPath);
        if (!watchedHandleIsAccessible || watchPoint.registeredFinalPath != currentFinalPath) {
            jstring javaPath = env->NewString((jchar*) wideToUtf16String(watchPoint.registeredPath).c_str(), (jsize) watchPoint.registeredPath.length());
            env->CallBooleanMethod(droppedPaths, listAddMethod, javaPath);
            env->DeleteLocalRef(javaPath);
            getJavaExceptionAndPrintStacktrace(env);

            watchPoint.cancel();
        }
    }
}

//
// JNI calls
//

JNIEXPORT jobject JNICALL
Java_org_gradle_fileevents_internal_WindowsFileEventFunctions_startWatcher0(JNIEnv* env, jclass, jint eventBufferSize, jlong commandTimeoutInMillis, jobject javaCallback) {
    return wrapServer(env, new Server(env, eventBufferSize, (long) commandTimeoutInMillis, javaCallback));
}

JNIEXPORT void JNICALL
Java_org_gradle_fileevents_internal_WindowsFileEventFunctions_00024WindowsFileWatcher_stopWatchingMovedPaths0(JNIEnv* env, jobject, jobject javaServer, jobject jDroppedPaths) {
    try {
        Server* server = (Server*) getServer(env, javaServer);
        server->stopWatchingMovedPaths(jDroppedPaths);
    } catch (const exception& e) {
        rethrowAsJavaException(env, e);
    }
}

#endif
