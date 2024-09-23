#include "thread.h"
#include "fs/FSUtils.h"
#include "retain_vars.hpp"
#include "screenshot_utils.h"
#include "utils/StringTools.h"
#include "utils/logger.h"
#include <coreinit/cache.h>
#include <coreinit/title.h>
#include <dirent.h>
#include <malloc.h>
#include <memory/mappedmemory.h>

#include <fstream>
#include <iostream>
#include <string>

#include <curl/curl.h>

FSIOThreadData gThreadData;
bool gThreadsRunning;

void upload_file(const std::string& api_key, const std::string& file_path, NotificationModuleHandle notificationHandle = 0) {
    CURL *curl;
    CURLcode res;
    NotificationModuleStatus err;
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;
    struct curl_slist *headers = NULL;

    std::string errorMessage = "Uploading screenshot to E-Z Host...";
    NotificationModule_AddDynamicNotification(errorMessage.c_str(), &notificationHandle);

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if(curl) {
        std::string api_key_header = "key: " + api_key;
        headers = curl_slist_append(headers, api_key_header.c_str());

        curl_formadd(&formpost,
                     &lastptr,
                     CURLFORM_COPYNAME, "file",
                     CURLFORM_FILE, file_path.c_str(),
                     CURLFORM_CONTENTTYPE, "application/octet-stream",
                     CURLFORM_FILENAME, file_path.c_str(),
                     CURLFORM_END);

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.e-z.host/files");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);

        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            std::string successMessage = "screenshot uploaded successfully!";
            NotificationModule_UpdateDynamicNotificationText(notificationHandle, successMessage.c_str());
            NotificationModule_FinishDynamicNotification(notificationHandle, 2.0f);
        } else {
            std::string errorMessage = "Error uploading screenshot: " + std::string(curl_easy_strerror(res));
            NotificationModule_UpdateDynamicNotificationText(notificationHandle, errorMessage.c_str());
            NotificationModule_UpdateDynamicNotificationBackgroundColor(notificationHandle, COLOR_RED);
            NotificationModule_FinishDynamicNotificationWithShake(notificationHandle, 2.0f, 0.5f);
        }

        curl_easy_cleanup(curl);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
    } else {
        std::string errorMessage = "Failed to initialize curl";
        NotificationModule_UpdateDynamicNotificationText(notificationHandle, errorMessage.c_str());
        NotificationModule_UpdateDynamicNotificationBackgroundColor(notificationHandle, COLOR_RED);
        NotificationModule_FinishDynamicNotificationWithShake(notificationHandle, 2.0f, 0.5f);
    }
    curl_global_cleanup();
}

static bool getPath(GX2ScanTarget scanTarget, ImageOutputFormatEnum outputFormat, std::string &path, OSCalendarTime &output) {
    std::string buffer = string_format("%s%016llX", WIIU_SCREENSHOT_PATH, OSGetTitleID());
    if (!gShortNameEn.empty()) {
        buffer += string_format(" (%s)", gShortNameEn.c_str());
    }
    buffer += string_format("/%04d-%02d-%02d/", output.tm_year, output.tm_mon + 1, output.tm_mday);

    auto dir = opendir(buffer.c_str());
    if (dir) {
        closedir(dir);
    } else {
        if (!FSUtils::CreateSubfolder(buffer.c_str())) {
            DEBUG_FUNCTION_LINE_ERR("Failed to create dir: %s", buffer.c_str());
            return false;
        }
    }

    path += string_format("%s%04d-%02d-%02d_%02d.%02d.%02d.%03d_",
                          buffer.c_str(), output.tm_year, output.tm_mon + 1,
                          output.tm_mday, output.tm_hour, output.tm_min, output.tm_sec, output.tm_msec);

    if (scanTarget == GX2_SCAN_TARGET_DRC) {
        path += "DRC";
    } else if (scanTarget == GX2_SCAN_TARGET_TV) {
        path += "TV";
    } else if (scanTarget == GX2_SCAN_TARGET_DRC1) {
        path += "DRC2";
    } else {
        DEBUG_FUNCTION_LINE_ERR("Invalid scanTarget %d", scanTarget);
        return false;
    }

    switch (outputFormat) {
        case IMAGE_OUTPUT_FORMAT_JPEG:
            path += ".jpg";
            break;
        case IMAGE_OUTPUT_FORMAT_PNG:
            path += ".png";
            break;
        case IMAGE_OUTPUT_FORMAT_BMP:
            path += ".bmp";
            break;
        default:
            DEBUG_FUNCTION_LINE_WARN("Invalid output format, use jpeg instead");
            path += ".jpg";
            break;
    }
    return true;
}

static int32_t fsIOThreadCallback([[maybe_unused]] int argc, const char **argv) {
    auto *magic = ((FSIOThreadData *) argv);

    constexpr int32_t messageSize = sizeof(magic->messages) / sizeof(magic->messages[0]);
    OSInitMessageQueue(&magic->queue, magic->messages, messageSize);
    OSMessage recv;
    while (OSReceiveMessage(&magic->queue, &recv, OS_MESSAGE_FLAGS_BLOCKING)) {
        if (recv.args[0] == FS_IO_QUEUE_COMMAND_STOP) {
            DEBUG_FUNCTION_LINE("Received break command! Stop thread");
            break;
        } else if (recv.args[0] == FS_IO_QUEUE_COMMAND_PROCESS_FS_COMMAND) {
            auto *message = (SaveScreenshotMessage *) recv.message;

            std::string path;
            bool success = false;
            if (getPath(message->scanTarget, message->outputFormat, path, message->time)) {
                DEBUG_FUNCTION_LINE("Saving to %s", path.c_str());
                auto res = saveTextureAsPicture(path, message->sourceBuffer, message->width, message->height, message->pitch, message->format, message->outputFormat, message->convertRGBtoSRGB, message->quality);
                if (res) {
                    NotificationModuleStatus err;
                    auto text = string_format("\ue01e Saving screenshot of the %s done!", message->scanTarget == GX2_SCAN_TARGET_TV ? "TV" : "GamePad");
                    if ((err = NotificationModule_UpdateDynamicNotificationText(message->notificationHandle, text.c_str())) != NOTIFICATION_MODULE_RESULT_SUCCESS ||
                        (err = NotificationModule_FinishDynamicNotification(message->notificationHandle, 2.0)) != NOTIFICATION_MODULE_RESULT_SUCCESS) {
                        DEBUG_FUNCTION_LINE_ERR("Failed to update notification: %s", NotificationModule_GetStatusStr(err));
                    }
                    upload_file("EZ_KEY", path, 0);
                    success = true;
                }
            } else {
                DEBUG_FUNCTION_LINE_ERR("Failed to get and create path");
            }
            if (!success) {
                NotificationModuleStatus err;
                auto errorText = string_format("\ue01e Saving screenshot of the %s failed", message->scanTarget == GX2_SCAN_TARGET_TV ? "TV" : "GamePad");
                if ((err = NotificationModule_UpdateDynamicNotificationText(message->notificationHandle, errorText.c_str())) != NOTIFICATION_MODULE_RESULT_SUCCESS ||
                    (err = NotificationModule_UpdateDynamicNotificationBackgroundColor(message->notificationHandle, COLOR_RED)) != NOTIFICATION_MODULE_RESULT_SUCCESS ||
                    (err = NotificationModule_FinishDynamicNotificationWithShake(message->notificationHandle, 2.0f, 0.5f)) != NOTIFICATION_MODULE_RESULT_SUCCESS) {
                    DEBUG_FUNCTION_LINE_ERR("Failed to update notification: %s", NotificationModule_GetStatusStr(err));
                }
            }

            // Free the colorbuffer copy.
            if (message->sourceBuffer != nullptr) {
                MEMFreeToMappedMemory(message->sourceBuffer);
            }

            OSMemoryBarrier();
            free(message);
        }
    }

    return 0;
}

bool sendMessageToThread(SaveScreenshotMessage *param) {
    auto *curThread = &gThreadData;
    if (curThread->setup) {
        OSMessage send;
        send.message = param;
        send.args[0] = FS_IO_QUEUE_COMMAND_PROCESS_FS_COMMAND;
        OSMemoryBarrier();
        return OSSendMessage(&curThread->queue, &send, OS_MESSAGE_FLAGS_NONE);
    } else {
        DEBUG_FUNCTION_LINE_ERR("Thread not setup");
    }
    return false;
}

void startFSIOThreads() {
    auto stackSize = 16 * 1024;

    auto *threadData = &gThreadData;
    memset(threadData, 0, sizeof(*threadData));
    threadData->setup  = false;
    threadData->thread = (OSThread *) memalign(8, sizeof(OSThread));
    if (!threadData->thread) {
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate threadData");
        OSFatal("ScreenshotPlugin: Failed to allocate IO Thread");
        return;
    }
    threadData->stack = (uint8_t *) memalign(0x20, stackSize);
    if (!threadData->thread) {
        free(threadData->thread);
        DEBUG_FUNCTION_LINE_ERR("Failed to allocate threadData stack");
        OSFatal("ScreenshotPlugin: Failed to allocate IO Thread stack");
        return;
    }

    OSMemoryBarrier();

    if (!OSCreateThread(threadData->thread, &fsIOThreadCallback, 1, (char *) threadData, reinterpret_cast<void *>((uint32_t) threadData->stack + stackSize), stackSize, 31, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        free(threadData->thread);
        free(threadData->stack);
        threadData->setup = false;
        DEBUG_FUNCTION_LINE_ERR("failed to create threadData");
        OSFatal("ContentRedirectionModule: Failed to create threadData");
    }

    strncpy(threadData->threadName, "ScreenshotPlugin IO Thread", sizeof(threadData->threadName) - 1);
    OSSetThreadName(threadData->thread, threadData->threadName);
    OSResumeThread(threadData->thread);
    threadData->setup = true;

    gThreadsRunning = true;
    OSMemoryBarrier();
}

void stopFSIOThreads() {
    if (!gThreadsRunning) {
        return;
    }
    auto *thread = &gThreadData;
    if (!thread->setup) {
        return;
    }
    OSMessage message;
    message.args[0] = FS_IO_QUEUE_COMMAND_STOP;
    OSSendMessage(&thread->queue, &message, OS_MESSAGE_FLAGS_BLOCKING);

    if (!OSSetThreadPriority(thread->thread, 0)) {
        DEBUG_FUNCTION_LINE_ERR("Failed to boost priority, the game might softlock");
    } else {
        DEBUG_FUNCTION_LINE_VERBOSE("Set priority to 0!");
    }
    if (OSIsThreadSuspended(thread->thread)) {
        OSResumeThread(thread->thread);
    }

    OSJoinThread(thread->thread, nullptr);
    if (thread->stack) {
        free(thread->stack);
        thread->stack = nullptr;
    }
    if (thread->thread) {
        free(thread->thread);
        thread->thread = nullptr;
    }

    gThreadsRunning = false;
}