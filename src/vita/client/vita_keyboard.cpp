// See vita_keyboard.h.

#include "vita_keyboard.h"

#include "vita_client.h"

#include <gfx_gxm/gxm_display.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>

#include <pthread.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

enum class Phase : uint8_t
{
    Idle,
    Asked, // waiting for the next frame drawn
    Up,    // the dialog is open
    Done,  // its result waits to be taken
};

// The lock guards these. No system call is made while it is held.
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
Phase s_phase = Phase::Idle;
VitaKeyboardRequest s_request;
VitaKeyboardResult s_result = VitaKeyboardResult::None;
uint16_t s_resultText[kVitaKeyboardMaxText + 1];
bool s_abort;
bool s_broken; // the system refused a keyboard: no more are asked for
bool s_hooked;

// The drawing thread's own. The dialog reads the three buffers while it is up.
SceWChar16 s_dialogTitle[kVitaKeyboardMaxTitle + 1];
SceWChar16 s_dialogInitial[kVitaKeyboardMaxText + 1];
SceWChar16 s_dialogText[kVitaKeyboardMaxText + 1];
bool s_configured;
bool s_abortSent;
bool s_updateFailureLogged;
SceUID s_dialogThread = -1;

struct Lock
{
    Lock() { pthread_mutex_lock(&s_lock); }
    ~Lock() { pthread_mutex_unlock(&s_lock); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    Vita_ClientLog(line);
}

void Finish(VitaKeyboardResult result, const SceWChar16 *text)
{
    Lock lock;
    s_result = result;
    s_resultText[0] = 0;
    if (result == VitaKeyboardResult::Entered && text)
    {
        int length = 0;
        for (; length < kVitaKeyboardMaxText && text[length]; ++length)
            s_resultText[length] = text[length];
        s_resultText[length] = 0;
    }
    s_phase = Phase::Done;
    s_abort = false;
}

// Once: the system's dialogs want to know the console's language and which
// button confirms.
void Configure()
{
    if (s_configured)
        return;
    s_configured = true;
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    if (result < 0)
        Log("vita keyboard: sceSysmoduleLoadModule(IME) said 0x%08x", static_cast<unsigned int>(result));
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    result = sceAppUtilInit(&init, &boot);
    if (result < 0)
        Log("vita keyboard: sceAppUtilInit said 0x%08x", static_cast<unsigned int>(result));
    SceCommonDialogConfigParam config;
    sceCommonDialogConfigParamInit(&config);
    int value = 0;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &value) >= 0)
        config.language = static_cast<SceSystemParamLang>(value);
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &value) >= 0)
        config.enterButtonAssign = static_cast<SceSystemParamEnterButtonAssign>(value);
    result = sceCommonDialogSetConfigParam(&config);
    if (result < 0)
        Log("vita keyboard: sceCommonDialogSetConfigParam said 0x%08x", static_cast<unsigned int>(result));
}

bool Open(const VitaKeyboardRequest &request)
{
    Configure();
    memcpy(s_dialogTitle, request.title, sizeof(s_dialogTitle));
    memcpy(s_dialogInitial, request.initial, sizeof(s_dialogInitial));
    s_dialogTitle[kVitaKeyboardMaxTitle] = 0;
    s_dialogInitial[kVitaKeyboardMaxText] = 0;
    memset(s_dialogText, 0, sizeof(s_dialogText));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.type = request.kind == VitaKeyboardKind::Number    ? SCE_IME_TYPE_NUMBER
                 : request.kind == VitaKeyboardKind::Decimal ? SCE_IME_TYPE_EXTENDED_NUMBER
                                                             : SCE_IME_TYPE_DEFAULT;
    // Names, addresses and commands: nothing is to be corrected or capitalised.
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = s_dialogTitle;
    param.maxTextLength = static_cast<SceUInt32>(request.maxLength < 1                      ? 1
                                                 : request.maxLength > kVitaKeyboardMaxText ? kVitaKeyboardMaxText
                                                                                            : request.maxLength);
    param.initialText = s_dialogInitial;
    param.inputTextBuffer = s_dialogText;
    // The console's own keyboards first. If the system does not take that,
    // the English one, which every console has.
    int result = sceImeDialogInit(&param);
    if (result < 0)
    {
        Log("vita keyboard: sceImeDialogInit said 0x%08x with the console's languages; trying English",
            static_cast<unsigned int>(result));
        param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
        param.languagesForced = SCE_TRUE;
        result = sceImeDialogInit(&param);
    }
    if (result < 0)
    {
        Log("vita keyboard: sceImeDialogInit failed (0x%08x); the on-screen keyboard is off for this run",
            static_cast<unsigned int>(result));
        return false;
    }
    s_abortSent = false;
    s_dialogThread = sceKernelGetThreadId();
    return true;
}

// On the drawing thread, between a frame's last scene and its flip.
void BeforeFlip(const gfx_gxm::FlipInfo &info)
{
    Phase phase;
    bool abort;
    VitaKeyboardRequest request;
    {
        Lock lock;
        phase = s_phase;
        abort = s_abort;
        if (phase == Phase::Asked)
            request = s_request;
    }
    if (phase == Phase::Asked)
    {
        if (abort)
        {
            Finish(VitaKeyboardResult::Cancelled, nullptr);
            return;
        }
        if (!Open(request))
        {
            Finish(VitaKeyboardResult::Failed, nullptr);
            return;
        }
        // Only this thread leaves Asked, and nobody asks again before the
        // result is taken.
        Lock lock;
        s_phase = phase = Phase::Up;
    }
    // Another thread is drawing (the main thread draws some frames itself):
    // the dialog waits for its own.
    if (phase != Phase::Up || sceKernelGetThreadId() != s_dialogThread)
        return;
    if (abort && !s_abortSent)
    {
        s_abortSent = true;
        sceImeDialogAbort();
    }

    // The system draws the keyboard into the frame.
    SceCommonDialogUpdateParam update;
    memset(&update, 0, sizeof(update));
    update.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    update.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    update.renderTarget.width = gfx_gxm::kDisplayWidth;
    update.renderTarget.height = gfx_gxm::kDisplayHeight;
    update.renderTarget.strideInPixels = gfx_gxm::kDisplayStride;
    update.renderTarget.colorSurfaceData = info.colorData;
    update.renderTarget.depthSurfaceData = info.depthStencilData;
    update.displaySyncObject = info.sync;
    const int updated = sceCommonDialogUpdate(&update);
    if (updated < 0 && !s_updateFailureLogged)
    {
        s_updateFailureLogged = true;
        Log("vita keyboard: sceCommonDialogUpdate said 0x%08x (logged once)", static_cast<unsigned int>(updated));
    }

    const SceCommonDialogStatus status = sceImeDialogGetStatus();
    if (status == SCE_COMMON_DIALOG_STATUS_RUNNING)
        return;
    bool entered = false;
    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED)
    {
        SceImeDialogResult result;
        memset(&result, 0, sizeof(result));
        entered = sceImeDialogGetResult(&result) >= 0 && result.button == SCE_IME_DIALOG_BUTTON_ENTER && !abort;
        s_dialogText[kVitaKeyboardMaxText] = 0;
    }
    else
    {
        Log("vita keyboard: the dialog is not running (status %d); taken as cancelled", static_cast<int>(status));
    }
    sceImeDialogTerm();
    Finish(entered ? VitaKeyboardResult::Entered : VitaKeyboardResult::Cancelled, s_dialogText);
}

} // namespace

bool Vita_AskKeyboard(const VitaKeyboardRequest &request)
{
    bool hook = false;
    {
        Lock lock;
        if (s_broken || s_phase != Phase::Idle)
            return false;
        s_request = request;
        s_request.title[kVitaKeyboardMaxTitle] = 0;
        s_request.initial[kVitaKeyboardMaxText] = 0;
        s_abort = false;
        s_phase = Phase::Asked;
        hook = !s_hooked;
        s_hooked = true;
    }
    if (hook)
        gfx_gxm::SetBeforeFlip(BeforeFlip);
    return true;
}

VitaKeyboardResult Vita_TakeKeyboard(uint16_t *text)
{
    Lock lock;
    if (s_phase == Phase::Idle)
        return VitaKeyboardResult::None;
    if (s_phase != Phase::Done)
        return VitaKeyboardResult::Waiting;
    const VitaKeyboardResult result = s_result;
    if (text)
        memcpy(text, s_resultText, sizeof(s_resultText));
    if (result == VitaKeyboardResult::Failed)
        s_broken = true;
    s_result = VitaKeyboardResult::None;
    s_phase = Phase::Idle;
    return result;
}

void Vita_AbortKeyboard()
{
    Lock lock;
    if (s_phase == Phase::Asked || s_phase == Phase::Up)
        s_abort = true;
}
