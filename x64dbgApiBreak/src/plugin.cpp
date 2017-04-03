#include <corelib.h>
#include <settings.h>
#include <dlgs/MainForm.hpp>
#include <dlgs/SettingsForm.hpp>
#include <structmemmap.h>


#define MN_BASE                 0xFA00FA

#define MN_ABOUT                MN_BASE
#define MN_SHOWMAINFORM         MN_BASE + 1
#define MN_SHOWSETTINGSFORM     MN_BASE + 2
#define MN_TESTSLOT             MN_BASE + 3

#define DWS_IDLE                0
#define DWS_CREATEPROCESS       1
#define DWS_ATTACHEDPROCESS     2
#define DWS_EXITEDPROCESS       3
#define DWS_DETACHEDPROCESS     4

const char *DBGSTATE_STRINGS[5] =
{
    "Idle",
    "Process Created",
    "Process Attached",
    "Process Exited",
    "Process Detached"
};


BYTE                                AbpDbgState;
Script::Module::ModuleInfo          AbpCurrentMainModule;
BOOL                                AbfNeedsReload=FALSE;

INTERNAL int                        AbPluginHandle;
INTERNAL HWND                       AbHwndDlgHandle;
INTERNAL int                        AbMenuHandle;
INTERNAL int                        AbMenuDisasmHandle;
INTERNAL int                        AbMenuDumpHandle;
INTERNAL int                        AbMenuStackHandle;
INTERNAL HMODULE                    AbPluginModule;

INTERNAL WORD                       AbParsedTypeCount = 0;

INTERNAL void AbiRaiseDeferredLoader(const char *dllName, duint base);

INTERNAL void AbiInitDynapi();
INTERNAL void AbiUninitDynapi();
INTERNAL void AbiReleaseDeferredResources();
INTERNAL void AbiEmptyInstructionCache();

BOOL                                AbpHasPendingInit = FALSE;

INTERNAL_EXPORT Script::Module::ModuleInfo *AbiGetCurrentModuleInfo()
{
    return &AbpCurrentMainModule;
}



int WINAPI Loader(void *)
{
    AbLoadAvailableModuleAPIs(true);
    return 0;
}

void AbpParseScripts()
{
    char *tokCtx;
    char *scripts = NULL;
    char *line;

    if (!AbGetSettings()->mainScripts)
        return;

    scripts = HlpCloneStringA(AbGetSettings()->mainScripts);

    if (!scripts)
    {
        DBGPRINT("memory error");
        return;
    }

    line = strtok_s(scripts, "\r\n;", &tokCtx);

    while (line)
    {
        DBGPRINT("Parsing '%s'", line);
        SmmParseFromFileA(line, &AbParsedTypeCount);
        line = strtok_s(NULL, "\r\n;", &tokCtx);
    }

    DBGPRINT("%d type(s) parsed.", AbParsedTypeCount);

    FREESTRING(line);
}

void AbpOnDebuggerStateChanged(BYTE state, const char *module)
{
    switch (state)
    {
        case DWS_ATTACHEDPROCESS:
        case DWS_CREATEPROCESS:
        {
            if (AbGetSettings()->autoLoadData)
            {
                AbpHasPendingInit = TRUE;
            }
        }
        break;
        case DWS_DETACHEDPROCESS:
        case DWS_EXITEDPROCESS:
        {
            AbReleaseAllSystemResources(false);
        }
        break;
    }
}

void AbpRaiseDbgStateChangeEvent()
{
    DBGPRINT("%s (%s)", AbpCurrentMainModule.name, DBGSTATE_STRINGS[AbpDbgState]);

    AbpOnDebuggerStateChanged(AbpDbgState, AbpCurrentMainModule.name);

    if (AbpDbgState > DWS_ATTACHEDPROCESS)
    {
        //And after the handler callback. 
        //set it to dbg is now idle if current state exited or detached
        AbpDbgState = DWS_IDLE;
    }
}

void AbReleaseAllSystemResources(bool isInShutdown)
{
    DBGPRINT("Releasing used resources. shutdown=%d",isInShutdown);

    AbiEmptyInstructionCache();
    AbiReleaseDeferredResources();
    AbpReleaseBreakpointResources();
    AbReleaseModuleResources();

    if (isInShutdown)
    {
        AbiUninitDynapi();
        UiForceCloseAllActiveWindows();
    }
}

void __AbpInitMenu()
{
    _plugin_menuaddentry(AbMenuHandle, MN_SHOWMAINFORM, "set an API breakpoint");
    _plugin_menuaddentry(AbMenuHandle, MN_SHOWSETTINGSFORM, "settings");
    _plugin_menuaddentry(AbMenuHandle, MN_ABOUT, "about?");
}


DBG_LIBEXPORT bool pluginit(PLUG_INITSTRUCT* initStruct)
{
    initStruct->sdkVersion = PLUG_SDKVERSION;
    initStruct->pluginVersion = MAKEWORD(AB_VERSION_MAJOR, AB_VERSION_MINOR);
    strcpy_s(initStruct->pluginName, 256, "Api Break");
    AbPluginHandle = initStruct->pluginHandle;

    AbSettingsLoad();
    AbiInitDynapi();

    return true;
}

DBG_LIBEXPORT bool plugstop()
{
    AbReleaseAllSystemResources(true);
    return true;
}

DBG_LIBEXPORT void plugsetup(PLUG_SETUPSTRUCT* setupStruct)
{
    AbHwndDlgHandle = setupStruct->hwndDlg;
    AbMenuHandle = setupStruct->hMenu;
    AbMenuDisasmHandle = setupStruct->hMenuDisasm;
    AbMenuDumpHandle = setupStruct->hMenuDump;
    AbMenuStackHandle = setupStruct->hMenuStack;

    __AbpInitMenu();

    AbpParseScripts();
}


#include <util.h>

INTERNAL ApiFunctionInfo *AbiGetAfi(const char *module, const char *afiName);


DBG_LIBEXPORT void CBMENUENTRY(CBTYPE cbType, PLUG_CB_MENUENTRY* info)
{
    if (info->hEntry == MN_ABOUT)
    {
        MessageBoxA(AbHwndDlgHandle,
            AB_APPNAME " - ver: " AB_VERSTR "\r\n"
            "build on: " AB_BUILD_TIME "\r\n\r\n"
            "by oguz (ozzy) kartal (2017)\r\n\r\n"
            "http://oguzkartal.net ;)",
            "About - " AB_APPTITLE, 
            MB_ICONINFORMATION);
    }
    else if (info->hEntry == MN_SHOWMAINFORM)
    {
        //Ps. Form object will be deleted automatically when the window closed.
        MainForm *mainForm = new MainForm();
        mainForm->ShowDialog();
    }
    else if (info->hEntry == MN_SHOWSETTINGSFORM)
    {
        SettingsForm *settingsForm = new SettingsForm();
        settingsForm->ShowDialog();
    }
}


DBG_LIBEXPORT void CBLOADDLL(CBTYPE cbType, PLUG_CB_LOADDLL *dllLoad)
{
    duint base = DbgModBaseFromName(dllLoad->modname);

    if (!base)
    {
        DBGPRINT("Could not get dll base fro %s", dllLoad->modname);
        return;
    }

    AbiRaiseDeferredLoader(dllLoad->modname, base);
}

DBG_LIBEXPORT void CBSYSTEMBREAKPOINT(CBTYPE cbType, PLUG_CB_SYSTEMBREAKPOINT *sysbp)
{
    if (AbpHasPendingInit)
    {
        AbpHasPendingInit = FALSE;
        QueueUserWorkItem((LPTHREAD_START_ROUTINE)Loader, NULL, WT_EXECUTEDEFAULT);
    }
}

PPASSED_PARAMETER_CONTEXT AbpExtractPassedParameterContext(REGDUMP *regdump, PFNSIGN fnSign, BOOL ipOnStack)
{
    SHORT argCount;
    PPASSED_PARAMETER_CONTEXT ppc;
    CALLCONVENTION conv;
    
    argCount = SmmGetArgumentCount(fnSign);

#ifdef _WIN64
    conv = Fastcall;
#else
    conv = Stdcall;
#endif

    if (!UtlExtractPassedParameters(argCount, conv, regdump,ipOnStack, &ppc))
    {
        DBGPRINT("Parameter extraction failed");
        return NULL;
    }

    return ppc;
}


void AbpGhostBreakpointHandler(BpCallbackContext *bpx)
{
}

LONG WINAPI AbpShowOutputArgumentQuestion(LPVOID p)
{
    PFNSIGN fnSign;
    BOOL willContinue = FALSE;
    PPASSED_PARAMETER_CONTEXT ppc=NULL;
    BASIC_INSTRUCTION_INFO instr;
    duint addr;
    char msgBuf[512];

    char *nativeData;

    BpCallbackContext *bpcb = (BpCallbackContext *)p;
    
    sprintf(msgBuf, "One of the parameters of the %s is marked as out. "
        "That Means, you need to execute the api, to get all parameter result correctly.\n\n"
        "Want to execute the API now?",bpcb->afi->name);

    willContinue = MessageBoxA(NULL, msgBuf, "Quest", MB_ICONQUESTION | MB_YESNO) == IDYES;

    
    if (willContinue)
    {
        ppc = (PPASSED_PARAMETER_CONTEXT)bpcb->user;

        //Is it alreadly backtraced for caller?
        if (!bpcb->backTrack)
        {
            //if not, we must do that.
            addr = UtlGetCallerAddress(&bpcb->regContext);

            DbgDisasmFastAt(addr, &instr);

            addr += instr.size;

            if (!AbSetInstructionBreakpoint(addr, AbpGhostBreakpointHandler, bpcb,true))
            {
                DBGPRINT("Bpx set failed");
                return EXIT_FAILURE;
            }
        }

        AbDebuggerRun();
        AbDebuggerWaitUntilPaused();

        SmmGetFunctionSignature2(bpcb->afi, &fnSign);

        SmmMapFunctionCall(ppc, fnSign, bpcb->afi);

    }



    return EXIT_SUCCESS;
}

DBG_LIBEXPORT void CBBREAKPOINT(CBTYPE cbType, PLUG_CB_BREAKPOINT* info)
{
    BpCallbackContext *bpcb = NULL;
    PBREAKPOINT_INFO pbi;
    BOOL isBacktrackBp;
    PPASSED_PARAMETER_CONTEXT ppc;
    PFNSIGN fnSign = NULL;

    pbi = AbpLookupBreakpoint(info->breakpoint->addr);

    bpcb = pbi->cbctx;

    if (pbi != NULL)
    {
        DBGPRINT("Special breakpoint detected.");

        pbi->hitCount++;

        if (bpcb != NULL)
        {
            DBGPRINT("Breakpoint has registered callback. Raising the breakpoint callback");

            //get current register context for current state
            DbgGetRegDump(&bpcb->regContext);

            bpcb->bp = info->breakpoint;

            if (bpcb->callback != NULL)
                bpcb->callback(bpcb);
        }

        if (AbGetSettings()->mapCallContext)
        {
            if (SmmGetFunctionSignature2(bpcb->afi, &fnSign))
            {
                DBGPRINT("Function mapping signature found. Mapping...");

                isBacktrackBp = (pbi->options & BPO_BACKTRACK) == BPO_BACKTRACK;

                ppc = AbpExtractPassedParameterContext(&bpcb->regContext, fnSign,!isBacktrackBp);

                if (SmmSigHasOutArgument(fnSign))
                {
                    DBGPRINT("%s has an out marked function. ", bpcb->afi->name);
                    bpcb->user = ppc;
                    QueueUserWorkItem((LPTHREAD_START_ROUTINE)AbpShowOutputArgumentQuestion, bpcb, WT_EXECUTELONGFUNCTION);
                }
                else
                    SmmMapFunctionCall(ppc, fnSign, bpcb->afi);
            }
        }

        if (pbi->options & BPO_SINGLESHOT)
            AbDeleteBreakpoint(pbi->addr);
    }
    else
    {
        if (AbpHasPendingInit)
        {
            AbpHasPendingInit = FALSE;
            QueueUserWorkItem((LPTHREAD_START_ROUTINE)Loader, NULL, WT_EXECUTEDEFAULT);
        }
    }
}

DBG_LIBEXPORT void CBCREATEPROCESS(CBTYPE cbType, PLUG_CB_CREATEPROCESS *newProc)
{
    if (AbpDbgState == DWS_ATTACHEDPROCESS)
    {
        //get module info. cuz we cant get the modinfo from in the attach callback 
        goto resumeExec;
    }

    AbpDbgState = DWS_CREATEPROCESS;

resumeExec:

    AbGetDebuggedModuleInfo(&AbpCurrentMainModule);
    AbfNeedsReload = TRUE;

    AbpRaiseDbgStateChangeEvent();
}

DBG_LIBEXPORT void CBEXITPROCESS(CBTYPE cbType, PLUG_CB_EXITPROCESS *exitProc)
{
    AbpDbgState = DWS_EXITEDPROCESS;

    AbpRaiseDbgStateChangeEvent();
}

DBG_LIBEXPORT void CBATTACH(CBTYPE cbType, PLUG_CB_ATTACH *attachedProc)
{
    AbpDbgState = DWS_ATTACHEDPROCESS;
}

DBG_LIBEXPORT void CBDETACH(CBTYPE cbType, PLUG_CB_DETACH *detachedProc)
{
    AbpDbgState = DWS_DETACHEDPROCESS;
    
    AbpRaiseDbgStateChangeEvent();
}

BOOL WINAPI DllMain(
    _In_ HINSTANCE hinstDLL,
    _In_ DWORD     fdwReason,
    _In_ LPVOID    lpvReserved
)
{
    AbPluginModule = (HMODULE)hinstDLL;
    return TRUE;
}