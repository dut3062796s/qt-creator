/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact:  Qt Software Information (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
**
**************************************************************************/

#include "cdbdebugengine.h"
#include "cdbdebugengine_p.h"
#include "cdbsymbolgroupcontext.h"
#include "cdbstacktracecontext.h"
#include "cdbbreakpoint.h"
#include "cdbmodules.h"
#include "cdbassembler.h"

#include "debuggeractions.h"
#include "debuggermanager.h"
#include "breakhandler.h"
#include "stackhandler.h"
#include "watchhandler.h"
#include "registerhandler.h"
#include "moduleshandler.h"
#include "watchutils.h"

#include <utils/qtcassert.h>
#include <utils/winutils.h>
#include <utils/consoleprocess.h>

#include <QtCore/QDebug>
#include <QtCore/QTimerEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QLibrary>
#include <QtCore/QCoreApplication>
#include <QtGui/QMessageBox>
#include <QtGui/QMainWindow>

#define DBGHELP_TRANSLATE_TCHAR
#include <inc/Dbghelp.h>

static const char *dbgEngineDllC = "dbgeng";
static const char *debugCreateFuncC = "DebugCreate";

static const char *localSymbolRootC = "local";

namespace Debugger {
namespace Internal {

typedef QList<WatchData> WatchList;

// ----- Message helpers

QString msgDebugEngineComResult(HRESULT hr)
{
    switch (hr) {
        case S_OK:
        return QLatin1String("S_OK");
        case S_FALSE:
        return QLatin1String("S_FALSE");
        case E_FAIL:
        break;
        case E_INVALIDARG:
        return QLatin1String("E_INVALIDARG");
        case E_NOINTERFACE:
        return QLatin1String("E_NOINTERFACE");
        case E_OUTOFMEMORY:
        return QLatin1String("E_OUTOFMEMORY");
        case E_UNEXPECTED:
        return QLatin1String("E_UNEXPECTED");
        case E_NOTIMPL:
        return QLatin1String("E_NOTIMPL");
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
        return QLatin1String("ERROR_ACCESS_DENIED");;
    if (hr == HRESULT_FROM_NT(STATUS_CONTROL_C_EXIT))
        return QLatin1String("STATUS_CONTROL_C_EXIT");
    return Core::Utils::winErrorMessage(HRESULT_CODE(hr));
}

static QString msgStackIndexOutOfRange(int idx, int size)
{
    return QString::fromLatin1("Frame index %1 out of range (%2).").arg(idx).arg(size);
}

QString msgComFailed(const char *func, HRESULT hr)
{
    return QString::fromLatin1("%1 failed: %2").arg(QLatin1String(func), msgDebugEngineComResult(hr));
}

static const char *msgNoStackTraceC = "Internal error: no stack trace present.";

// ----- Engine helpers

static inline ULONG getInterruptTimeOutSecs(IDebugControl4 *ctl)
{
    ULONG rc = 0;
    ctl->GetInterruptTimeout(&rc);
    return rc;
}

static inline bool getExecutionStatus(IDebugControl4 *ctl,
                                      ULONG *executionStatus,
                                      QString *errorMessage)
{
    const HRESULT hr = ctl->GetExecutionStatus(executionStatus);
    if (FAILED(hr)) {
        *errorMessage = msgComFailed("GetExecutionStatus", hr);
        return false;
    }
    return true;
}

// --------- DebuggerEngineLibrary
DebuggerEngineLibrary::DebuggerEngineLibrary() :
    m_debugCreate(0)
{
}

bool DebuggerEngineLibrary::init(QString *errorMessage)
{
    // Load
    QLibrary lib(QLatin1String(dbgEngineDllC), 0);

    if (!lib.isLoaded() && !lib.load()) {
        *errorMessage = CdbDebugEngine::tr("Unable to load the debugger engine library '%1': %2").
                        arg(QLatin1String(dbgEngineDllC), lib.errorString());
        return false;
    }
    // Locate symbols
    void *createFunc = lib.resolve(debugCreateFuncC);
    if (!createFunc) {
        *errorMessage = CdbDebugEngine::tr("Unable to resolve '%1' in the debugger engine library '%2'").
                        arg(QLatin1String(debugCreateFuncC), QLatin1String(dbgEngineDllC));
        return false;
    }
    m_debugCreate = static_cast<DebugCreateFunction>(createFunc);
    return true;
}

// A class that sets an expression syntax on the debug control while in scope.
// Can be nested as it checks for the old value.
class SyntaxSetter {
    Q_DISABLE_COPY(SyntaxSetter)
public:
    explicit inline SyntaxSetter(IDebugControl4 *ctl, ULONG desiredSyntax);
    inline  ~SyntaxSetter();
private:
    const ULONG m_desiredSyntax;
    IDebugControl4 *m_ctl;
    ULONG m_oldSyntax;
};

SyntaxSetter::SyntaxSetter(IDebugControl4 *ctl, ULONG desiredSyntax) :
    m_desiredSyntax(desiredSyntax),
    m_ctl(ctl)
{
    m_ctl->GetExpressionSyntax(&m_oldSyntax);
    if (m_oldSyntax != m_desiredSyntax)
        m_ctl->SetExpressionSyntax(m_desiredSyntax);
}

SyntaxSetter::~SyntaxSetter()
{
    if (m_oldSyntax != m_desiredSyntax)
        m_ctl->SetExpressionSyntax(m_oldSyntax);
}

// --- CdbDebugEnginePrivate

CdbDebugEnginePrivate::CdbDebugEnginePrivate(DebuggerManager *parent, CdbDebugEngine* engine) :
    m_hDebuggeeProcess(0),
    m_hDebuggeeThread(0),
    m_breakEventMode(BreakEventHandle),
    m_watchTimer(-1),
    m_debugEventCallBack(engine),
    m_debugOutputCallBack(engine),    
    m_pDebugClient(0),
    m_pDebugControl(0),
    m_pDebugSystemObjects(0),
    m_pDebugSymbols(0),
    m_pDebugRegisters(0),
    m_engine(engine),
    m_debuggerManager(parent),
    m_debuggerManagerAccess(parent->engineInterface()),
    m_currentStackTrace(0),
    m_firstActivatedFrame(true),
    m_mode(AttachCore)
{   
}

bool CdbDebugEnginePrivate::init(QString *errorMessage)
{
    // Load the DLL
    DebuggerEngineLibrary lib;
    if (!lib.init(errorMessage))
        return false;

    // Initialize the COM interfaces
    HRESULT hr;
    hr = lib.debugCreate( __uuidof(IDebugClient5), reinterpret_cast<void**>(&m_pDebugClient));
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Creation of IDebugClient5 failed: %1").arg(msgDebugEngineComResult(hr));
        return false;
    }

    m_pDebugClient->SetOutputCallbacks(&m_debugOutputCallBack);
    m_pDebugClient->SetEventCallbacks(&m_debugEventCallBack);

    hr = lib.debugCreate( __uuidof(IDebugControl4), reinterpret_cast<void**>(&m_pDebugControl));
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Creation of IDebugControl4 failed: %1").arg(msgDebugEngineComResult(hr));
        return false;
    }

    m_pDebugControl->SetCodeLevel(DEBUG_LEVEL_SOURCE);

    hr = lib.debugCreate( __uuidof(IDebugSystemObjects4), reinterpret_cast<void**>(&m_pDebugSystemObjects));
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Creation of IDebugSystemObjects4 failed: %1").arg(msgDebugEngineComResult(hr));
        return false;
    }

    hr = lib.debugCreate( __uuidof(IDebugSymbols3), reinterpret_cast<void**>(&m_pDebugSymbols));
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Creation of IDebugSymbols3 failed: %1").arg(msgDebugEngineComResult(hr));
        return false;
    }

    hr = lib.debugCreate( __uuidof(IDebugRegisters2), reinterpret_cast<void**>(&m_pDebugRegisters));
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Creation of IDebugRegisters2 failed: %1").arg(msgDebugEngineComResult(hr));
        return false;    
    }
    if (debugCDB)
        qDebug() << QString::fromLatin1("CDB Initialization succeeded, interrupt time out %1s.").arg(getInterruptTimeOutSecs(m_pDebugControl));
    return true;
}

IDebuggerEngine *CdbDebugEngine::create(DebuggerManager *parent)
{
    QString errorMessage;
    IDebuggerEngine *rc = 0;
    CdbDebugEngine *e = new CdbDebugEngine(parent);
    if (e->m_d->init(&errorMessage)) {
        rc = e;
    } else {
        delete e;
        qWarning("%s", qPrintable(errorMessage));
    }
    return rc;
}

CdbDebugEnginePrivate::~CdbDebugEnginePrivate()
{
    cleanStackTrace();
    if (m_pDebugClient)
        m_pDebugClient->Release();
    if (m_pDebugControl)
        m_pDebugControl->Release();
    if (m_pDebugSystemObjects)
        m_pDebugSystemObjects->Release();
    if (m_pDebugSymbols)
        m_pDebugSymbols->Release();
    if (m_pDebugRegisters)
        m_pDebugRegisters->Release();
}

void CdbDebugEnginePrivate::clearForRun()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    m_breakEventMode = BreakEventHandle;
    m_firstActivatedFrame = false;
    cleanStackTrace();
}

void CdbDebugEnginePrivate::cleanStackTrace()
{    
    if (m_currentStackTrace) {
        delete m_currentStackTrace;
        m_currentStackTrace = 0;
    }
}

CdbDebugEngine::CdbDebugEngine(DebuggerManager *parent) :
    IDebuggerEngine(parent),
    m_d(new CdbDebugEnginePrivate(parent, this))
{
    // m_d->m_consoleStubProc.setDebug(true);
    connect(&m_d->m_consoleStubProc, SIGNAL(processError(QString)), this, SLOT(slotConsoleStubError(QString)));
    connect(&m_d->m_consoleStubProc, SIGNAL(processStarted()), this, SLOT(slotConsoleStubStarted()));
    connect(&m_d->m_consoleStubProc, SIGNAL(wrapperStopped()), this, SLOT(slotConsoleStubTerminated()));
    connect(&m_d->m_debugOutputCallBack, SIGNAL(debuggerOutput(QString,QString)),
            m_d->m_debuggerManager, SLOT(showDebuggerOutput(QString,QString)));
    connect(&m_d->m_debugOutputCallBack, SIGNAL(debuggerInputPrompt(QString,QString)),
            m_d->m_debuggerManager, SLOT(showDebuggerInput(QString,QString)));
}

CdbDebugEngine::~CdbDebugEngine()
{
    delete m_d;
}

void CdbDebugEngine::startWatchTimer()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    if (m_d->m_watchTimer == -1)
        m_d->m_watchTimer = startTimer(0);
}

void CdbDebugEngine::killWatchTimer()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    if (m_d->m_watchTimer != -1) {
        killTimer(m_d->m_watchTimer);
        m_d->m_watchTimer = -1;
    }
}

void CdbDebugEngine::shutdown()
{
    exitDebugger();
}

void CdbDebugEngine::setToolTipExpression(const QPoint & /*pos*/, const QString & /*exp*/)
{
}

void CdbDebugEnginePrivate::clearDisplay()
{
    m_debuggerManagerAccess->threadsHandler()->removeAll();
    m_debuggerManagerAccess->modulesHandler()->removeAll();
    m_debuggerManagerAccess->registerHandler()->removeAll();
}

bool CdbDebugEngine::startDebugger()
{    
    m_d->clearDisplay();
    m_d->m_debuggerManager->showStatusMessage("Starting Debugger", -1);
    QString errorMessage;
    bool rc = false;
    m_d->clearForRun();
    const DebuggerStartMode mode = m_d->m_debuggerManager->startMode();
    switch (mode) {
    case AttachExternal:
        rc = startAttachDebugger(m_d->m_debuggerManager->m_attachedPID, &errorMessage);
        break;
    case StartInternal:
    case StartExternal:
        if (m_d->m_debuggerManager->m_useTerminal) {
            // Launch console stub and wait for its startup
            m_d->m_consoleStubProc.stop(); // We leave the console open, so recycle it now.
            m_d->m_consoleStubProc.setWorkingDirectory(m_d->m_debuggerManager->m_workingDir);
            m_d->m_consoleStubProc.setEnvironment(m_d->m_debuggerManager->m_environment);
            rc = m_d->m_consoleStubProc.start(m_d->m_debuggerManager->m_executable, m_d->m_debuggerManager->m_processArgs);
            if (!rc)
                errorMessage = tr("The console stub process was unable to start '%1'.").arg(m_d->m_debuggerManager->m_executable);
        } else {
            rc = startDebuggerWithExecutable(mode, &errorMessage);
        }
        break;
    case AttachCore:
        errorMessage = tr("CdbDebugEngine: Attach to core not supported!");
        break;
    }
    if (rc) {
        m_d->m_debuggerManager->showStatusMessage(tr("Debugger Running"), -1);
        startWatchTimer();
    } else {
        qWarning("%s\n", qPrintable(errorMessage));
    }
    return rc;
}

bool CdbDebugEngine::startAttachDebugger(qint64 pid, QString *errorMessage)
{
    // Need to aatrach invasively, otherwise, no notification signals
    // for for CreateProcess/ExitProcess occur.
    const HRESULT hr = m_d->m_pDebugClient->AttachProcess(NULL, pid,
                                                          DEBUG_ATTACH_INVASIVE_RESUME_PROCESS);
    if (debugCDB)
        qDebug() << "Attaching to " << pid << " returns " << hr;
    if (FAILED(hr)) {
        *errorMessage = tr("AttachProcess failed for pid %1: %2").arg(pid).arg(msgDebugEngineComResult(hr));
        return false;
    } else {
        m_d->m_mode = AttachExternal;
    }
    return true;
}

bool CdbDebugEngine::startDebuggerWithExecutable(DebuggerStartMode sm, QString *errorMessage)
{
    m_d->m_debuggerManager->showStatusMessage("Starting Debugger", -1);

    DEBUG_CREATE_PROCESS_OPTIONS dbgopts;
    memset(&dbgopts, 0, sizeof(dbgopts));
    dbgopts.CreateFlags = DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS;

    const QString filename(m_d->m_debuggerManager->m_executable);
    if (debugCDB)
        qDebug() << Q_FUNC_INFO <<filename;

    const QFileInfo fi(filename);
    m_d->m_pDebugSymbols->AppendImagePathWide(QDir::toNativeSeparators(fi.absolutePath()).utf16());
    //m_pDebugSymbols->SetSymbolOptions(SYMOPT_CASE_INSENSITIVE | SYMOPT_UNDNAME | SYMOPT_DEBUG | SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_AUTO_PUBLICS);
    m_d->m_pDebugSymbols->SetSymbolOptions(SYMOPT_CASE_INSENSITIVE | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_AUTO_PUBLICS);
    //m_pDebugSymbols->AddSymbolOptions(SYMOPT_CASE_INSENSITIVE | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG | SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_AUTO_PUBLICS | SYMOPT_NO_IMAGE_SEARCH);

    // TODO console
    const QString cmd = Core::Utils::AbstractProcess::createWinCommandline(filename, m_d->m_debuggerManager->m_processArgs);
    if (debugCDB)
        qDebug() << "Starting " << cmd;
    PCWSTR env = 0;
    QByteArray envData;
    if (!m_d->m_debuggerManager->m_environment.empty()) {
        envData = Core::Utils::AbstractProcess::createWinEnvironment(Core::Utils::AbstractProcess::fixWinEnvironment(m_d->m_debuggerManager->m_environment));
        env = reinterpret_cast<PCWSTR>(envData.data());
    }
    const HRESULT hr = m_d->m_pDebugClient->CreateProcess2Wide(NULL,
                                                               const_cast<PWSTR>(cmd.utf16()),
                                                               &dbgopts,
                                                               sizeof(dbgopts),
                                                               m_d->m_debuggerManager->m_workingDir.utf16(),
                                                               env);
    if (FAILED(hr)) {
        *errorMessage = tr("CreateProcess2Wide failed for '%1': %2").arg(cmd, msgDebugEngineComResult(hr));
        m_d->m_debuggerManagerAccess->notifyInferiorExited();
        return false;
    } else {
        m_d->m_mode = sm;
    }
    m_d->m_debuggerManagerAccess->notifyInferiorRunning();
    return true;
}

void CdbDebugEngine::processTerminated(unsigned long exitCode)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << exitCode;

    m_d->clearForRun();
    m_d->setDebuggeeHandles(0, 0);
    m_d->m_debuggerManagerAccess->notifyInferiorExited();
    m_d->m_debuggerManager->exitDebugger();
}

void CdbDebugEngine::exitDebugger()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    if (m_d->m_hDebuggeeProcess) {
        QString errorMessage;
        m_d->clearForRun();
        bool wasRunning = false;
        // Terminate or detach if we are running
        HRESULT hr;
        switch (m_d->m_mode) {
        case AttachExternal:            
            wasRunning = m_d->isDebuggeeRunning();
            if (wasRunning) { // Process must be stopped in order to detach
                m_d->interruptInterferiorProcess(&errorMessage);
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            hr = m_d->m_pDebugClient->DetachCurrentProcess();
            if (FAILED(hr))
                errorMessage += msgComFailed("DetachCurrentProcess", hr);
            if (debugCDB)
                qDebug() << Q_FUNC_INFO << "detached" << msgDebugEngineComResult(hr);
            break;
        case StartExternal:
        case StartInternal:            
            wasRunning = m_d->isDebuggeeRunning();
            if (wasRunning) { // Process must be stopped in order to terminate
                m_d->interruptInterferiorProcess(&errorMessage);
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            // Terminate and wait for stop events.
            hr = m_d->m_pDebugClient->TerminateCurrentProcess();
            if (FAILED(hr))
                errorMessage += msgComFailed("TerminateCurrentProcess", hr);
            if (!wasRunning) {
                hr = m_d->m_pDebugClient->TerminateProcesses();
                if (FAILED(hr))
                    errorMessage += msgComFailed("TerminateProcesses", hr);
            }
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            if (debugCDB)
                qDebug() << Q_FUNC_INFO << "terminated" << msgDebugEngineComResult(hr);

            break;
        case AttachCore:
            break;
        }
        m_d->setDebuggeeHandles(0, 0);
        if (!errorMessage.isEmpty())
            qWarning("exitDebugger: %s\n", qPrintable(errorMessage));
    }
    killWatchTimer();
}

CdbSymbolGroupContext *CdbDebugEnginePrivate::getStackFrameSymbolGroupContext(int frameIndex, QString *errorMessage) const
{
    if (!m_currentStackTrace) {
        *errorMessage = QLatin1String(msgNoStackTraceC);
        return 0;
    }
    if (CdbSymbolGroupContext *sg = m_currentStackTrace->symbolGroupContextAt(frameIndex, errorMessage))
        return sg;
    return 0;
}

static inline QString formatWatchList(const WatchList &wl)
{
    const int count = wl.size();
    QString rc;
    for (int i = 0; i < count; i++) {
        if (i)
            rc += QLatin1String(", ");
        rc += wl.at(i).iname;
        rc += QLatin1String(" (");
        rc += wl.at(i).exp;
        rc += QLatin1Char(')');
    }
    return rc;
}

bool CdbDebugEnginePrivate::updateLocals(int frameIndex,
                                         WatchHandler *wh,
                                         QString *errorMessage)
{
    wh->reinitializeWatchers();

    QList<WatchData> incompletes = wh->takeCurrentIncompletes();
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << "\n    " << frameIndex << formatWatchList(incompletes);

    m_engine->filterEvaluateWatchers(&incompletes, wh);
    if (!incompletes.empty()) {        
        const QString msg = QLatin1String("Warning: Locals left in incomplete list: ") + formatWatchList(incompletes);
        qWarning("%s\n", qPrintable(msg));
    }

    bool success = false;
    if (CdbSymbolGroupContext *sgc = getStackFrameSymbolGroupContext(frameIndex, errorMessage))
        success = CdbSymbolGroupContext::populateModelInitially(sgc, wh, errorMessage);

    wh->rebuildModel();
    return success;
}

void CdbDebugEngine::evaluateWatcher(WatchData *wd)
{
    if (debugCDB > 1)
        qDebug() << Q_FUNC_INFO << wd->exp;
    QString errorMessage;
    QString value;
    QString type;
    if (evaluateExpression(wd->exp, &value, &type, &errorMessage)) {
        wd->setValue(value);
        wd->setType(type);
    } else {
        wd->setValue(errorMessage);
        wd->setTypeUnneeded();
    }
    wd->setChildCount(0);
}

void CdbDebugEngine::filterEvaluateWatchers(QList<WatchData> *wd, WatchHandler *wh)
{
    typedef QList<WatchData> WatchList;
    if (wd->empty())
        return;

    // Filter out actual watchers. Ignore the "<Edit>" top level place holders
    SyntaxSetter syntaxSetter(m_d->m_pDebugControl, DEBUG_EXPR_CPLUSPLUS);
    const QString watcherPrefix = QLatin1String("watch.");
    const QChar lessThan = QLatin1Char('<');
    const QChar greaterThan = QLatin1Char('>');
    bool placeHolderSeen = false;
    for (WatchList::iterator it = wd->begin(); it != wd->end(); ) {
        if (it->iname.startsWith(watcherPrefix)) {
            const bool isPlaceHolder = it->exp.startsWith(lessThan) && it->exp.endsWith(greaterThan);            
            if (isPlaceHolder) {
                if (!placeHolderSeen) { // Max one place holder
                    it->setChildCount(0);
                    it->setAllUnneeded();
                    wh->insertData(*it);
                    placeHolderSeen = true;
                }
            } else {
                evaluateWatcher(&(*it));
                wh->insertData(*it);
            }            
            it = wd->erase(it);
        } else {
            ++it;
        }
    }
}

void CdbDebugEngine::updateWatchModel()
{
    // Stack trace exists and evaluation funcs can only be called
    // when running
    if (m_d->isDebuggeeRunning()) {
        qWarning("updateWatchModel() called while debuggee is running.");
        return;
    }

    const int frameIndex = m_d->m_debuggerManagerAccess->stackHandler()->currentIndex();

    WatchHandler *watchHandler = m_d->m_debuggerManagerAccess->watchHandler();
    WatchList incomplete = watchHandler->takeCurrentIncompletes();
    if (incomplete.empty())
        return;
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << "\n    fi=" << frameIndex << formatWatchList(incomplete);

    bool success = false;
    QString errorMessage;
    do {
        // Filter out actual watchers
        filterEvaluateWatchers(&incomplete, watchHandler);
        // Do locals. We might get called while running when someone enters watchers
        if (!incomplete.empty()) {
            CdbSymbolGroupContext *sg = m_d->m_currentStackTrace->symbolGroupContextAt(frameIndex, &errorMessage);
            if (!sg || !CdbSymbolGroupContext::completeModel(sg, incomplete, watchHandler, &errorMessage))
                break;
        }
        watchHandler->rebuildModel();
        success = true;
    } while (false);
    if (!success)
        qWarning("%s : %s", Q_FUNC_INFO, qPrintable(errorMessage));
}

void CdbDebugEngine::stepExec()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    m_d->clearForRun();
    const HRESULT hr = m_d->m_pDebugControl->SetExecutionStatus(DEBUG_STATUS_STEP_INTO);
    Q_UNUSED(hr)

    m_d->m_breakEventMode = CdbDebugEnginePrivate::BreakEventIgnoreOnce;
    startWatchTimer();
}

void CdbDebugEngine::stepOutExec()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    StackHandler* sh = m_d->m_debuggerManagerAccess->stackHandler();
    const int idx = sh->currentIndex() + 1;
    QList<StackFrame> stackframes = sh->frames();
    if (idx < 0 || idx >= stackframes.size()) {
        qWarning("cannot step out");
        return;
    }

    // Set a temporary breakpoint and continue
    const StackFrame& frame = stackframes.at(idx);
    bool success = false;
    QString errorMessage;
    do {
        const ULONG64 address = frame.address.toULongLong(&success, 16);
        if (!success) {
            errorMessage = QLatin1String("Cannot obtain address from stack frame");
            break;
        }

        IDebugBreakpoint2* pBP;
        HRESULT hr = m_d->m_pDebugControl->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &pBP);
        if (FAILED(hr) || !pBP) {
            errorMessage = QString::fromLatin1("Cannot create temporary breakpoint: %1").arg(msgDebugEngineComResult(hr));
            break;
        }

        pBP->SetOffset(address);
        pBP->AddFlags(DEBUG_BREAKPOINT_ENABLED);
        pBP->AddFlags(DEBUG_BREAKPOINT_ONE_SHOT);
        if (!m_d->continueInferior(&errorMessage))
            break;
        success = true;
    } while (false);
    if (!success)
        qWarning("stepOutExec: %s\n", qPrintable(errorMessage));
}

void CdbDebugEngine::nextExec()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    m_d->clearForRun();
    const HRESULT hr = m_d->m_pDebugControl->SetExecutionStatus(DEBUG_STATUS_STEP_OVER);
    if (SUCCEEDED(hr)) {
        startWatchTimer();
    } else {
        qWarning("%s failed: %s", Q_FUNC_INFO, qPrintable(msgDebugEngineComResult(hr)));
    }
}

void CdbDebugEngine::stepIExec()
{
    qWarning("CdbDebugEngine::stepIExec() not implemented");
}

void CdbDebugEngine::nextIExec()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;

    m_d->clearForRun();
    const HRESULT hr = m_d->m_pDebugControl->Execute(DEBUG_OUTCTL_THIS_CLIENT, "p", 0);
    if (SUCCEEDED(hr)) {
        startWatchTimer();
    } else {
        qWarning("%s failed: %s", Q_FUNC_INFO, qPrintable(msgDebugEngineComResult(hr)));
    }
}

void CdbDebugEngine::continueInferior()
{
    QString errorMessage;
    if  (!m_d->continueInferior(&errorMessage))
        qWarning("continueInferior: %s\n", qPrintable(errorMessage));
}

// Continue process without notifications
bool CdbDebugEnginePrivate::continueInferiorProcess(QString *errorMessage)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
    const HRESULT hr = m_pDebugControl->SetExecutionStatus(DEBUG_STATUS_GO);
    if (FAILED(hr)) {
        *errorMessage = msgComFailed("SetExecutionStatus", hr);
        return false;
    }
    return  true;
}

// Continue process with notifications
bool CdbDebugEnginePrivate::continueInferior(QString *errorMessage)
{   
    ULONG executionStatus;
    if (!getExecutionStatus(m_pDebugControl, &executionStatus, errorMessage))
        return false;

    if (debugCDB)
        qDebug() << Q_FUNC_INFO << "\n    ex=" << executionStatus;

    if (executionStatus == DEBUG_STATUS_GO) {
        qWarning("continueInferior() called while debuggee is running.");
        return true;
    }

    clearForRun();
    m_engine->killWatchTimer();
    m_debuggerManager->resetLocation();
    m_debuggerManagerAccess->notifyInferiorRunningRequested();

    if (!continueInferiorProcess(errorMessage))
        return false;

    m_engine->startWatchTimer();
    m_debuggerManagerAccess->notifyInferiorRunning();
    return true;
}

bool CdbDebugEnginePrivate::interruptInterferiorProcess(QString *errorMessage)
{
    // Interrupt the interferior process without notifications
    if (debugCDB) {
        ULONG executionStatus;
        getExecutionStatus(m_pDebugControl, &executionStatus, errorMessage);
        qDebug() << Q_FUNC_INFO << "\n    ex=" << executionStatus;
    }

    if (!DebugBreakProcess(m_hDebuggeeProcess)) {
        *errorMessage = QString::fromLatin1("DebugBreakProcess failed: %1").arg(Core::Utils::winErrorMessage(GetLastError()));
        return false;
    }
#if 0    
    const HRESULT hr = m_pDebugControl->SetInterrupt(DEBUG_INTERRUPT_ACTIVE|DEBUG_INTERRUPT_EXIT);
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Unable to interrupt debuggee after %1s: %2").
                        arg(getInterruptTimeOutSecs(m_pDebugControl)).arg(msgComFailed("SetInterrupt", hr));
        return false;
    }
#endif
    return true;
}

void CdbDebugEngine::interruptInferior()
{
    if (!m_d->m_hDebuggeeProcess || !m_d->isDebuggeeRunning())
        return;

    QString errorMessage;
    if (m_d->interruptInterferiorProcess(&errorMessage)) {
        m_d->m_debuggerManagerAccess->notifyInferiorStopped();
    } else {
        qWarning("interruptInferior: %s\n", qPrintable(errorMessage));
    }
}

void CdbDebugEngine::runToLineExec(const QString &fileName, int lineNumber)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << fileName << lineNumber;
}

void CdbDebugEngine::runToFunctionExec(const QString &functionName)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << functionName;
}

void CdbDebugEngine::jumpToLineExec(const QString &fileName, int lineNumber)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << fileName << lineNumber;
}

void CdbDebugEngine::assignValueInDebugger(const QString &expr, const QString &value)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << expr << value;
    const int frameIndex = m_d->m_debuggerManagerAccess->stackHandler()->currentIndex();
    QString errorMessage;    
    bool success = false;
    do {
        QString newValue;
        CdbSymbolGroupContext *sg = m_d->getStackFrameSymbolGroupContext(frameIndex, &errorMessage);
        if (!sg)
            break;
        if (!sg->assignValue(expr, value, &newValue, &errorMessage))
            break;        
        // Update view
        WatchHandler *watchHandler = m_d->m_debuggerManagerAccess->watchHandler();
        if (WatchData *fwd = watchHandler->findData(expr)) {
            fwd->setValue(newValue);
            watchHandler->insertData(*fwd);
            watchHandler->rebuildModel();
        }
        success = true;
    } while (false);
    if (!success) {
        const QString msg = tr("Unable to assign the value '%1' to '%2': %3").arg(value, expr, errorMessage);
        qWarning("%s\n", qPrintable(msg));
    }
}

void CdbDebugEngine::executeDebuggerCommand(const QString &command)
{
    QString errorMessage;
    if (!executeDebuggerCommand(command, &errorMessage))
        qWarning("%s\n", qPrintable(errorMessage));
}

bool CdbDebugEngine::executeDebuggerCommand(const QString &command, QString *errorMessage)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << command;
    const HRESULT hr = m_d->m_pDebugControl->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, command.utf16(), 0);
    if (FAILED(hr)) {
        *errorMessage = QString::fromLatin1("Unable to execute '%1': %2").
                        arg(command, msgDebugEngineComResult(hr));
        return false;
    }
    return true;
}

bool CdbDebugEngine::evaluateExpression(const QString &expression,
                                        QString *value,
                                        QString *type,
                                        QString *errorMessage)
{
    if (debugCDB > 1)
        qDebug() << Q_FUNC_INFO << expression;
    DEBUG_VALUE debugValue;
    memset(&debugValue, 0, sizeof(DEBUG_VALUE));
    // Original syntax must be restored, else setting breakpoints will fail.
    SyntaxSetter syntaxSetter(m_d->m_pDebugControl, DEBUG_EXPR_CPLUSPLUS);
    ULONG errorPosition = 0;
    const HRESULT hr = m_d->m_pDebugControl->EvaluateWide(expression.utf16(),
                                                          DEBUG_VALUE_INVALID, &debugValue,
                                                          &errorPosition);    if (FAILED(hr)) {
        if (HRESULT_CODE(hr) == 517) {
            *errorMessage = QString::fromLatin1("Unable to evaluate '%1': Expression out of scope.").
                            arg(expression);
        } else {
            *errorMessage = QString::fromLatin1("Unable to evaluate '%1': Error at %2: %3").
                            arg(expression).arg(errorPosition).arg(msgDebugEngineComResult(hr));
        }
        return false;
    }
    *value = CdbSymbolGroupContext::debugValueToString(debugValue, m_d->m_pDebugControl, type);
    return true;
}

void CdbDebugEngine::activateFrame(int frameIndex)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << frameIndex;

    if (m_d->m_debuggerManager->status() != DebuggerInferiorStopped) {
        qWarning("WARNING %s: invoked while debuggee is running\n", Q_FUNC_INFO);
        return;
    }

    QString errorMessage;
    bool success = false;
    do {
        StackHandler *stackHandler = m_d->m_debuggerManagerAccess->stackHandler();
        WatchHandler *watchHandler = m_d->m_debuggerManagerAccess->watchHandler();
        const int oldIndex = stackHandler->currentIndex();
        if (frameIndex >= stackHandler->stackSize()) {
            errorMessage = msgStackIndexOutOfRange(frameIndex, stackHandler->stackSize());
            break;
        }

        if (oldIndex != frameIndex)
            stackHandler->setCurrentIndex(frameIndex);

        const StackFrame &frame = stackHandler->currentFrame();
        if (!frame.isUsable()) {
            // Clean out model
            watchHandler->reinitializeWatchers();
            watchHandler->rebuildModel();
            errorMessage = QString::fromLatin1("%1: file %2 unusable.").
                           arg(QLatin1String(Q_FUNC_INFO), frame.file);
            break;
        }

        if (oldIndex != frameIndex || m_d->m_firstActivatedFrame)
            if (!m_d->updateLocals(frameIndex, watchHandler, &errorMessage))
                break;

        m_d->m_debuggerManager->gotoLocation(frame.file, frame.line, true);
        success =true;
    } while (false);
    if (!success)
        qWarning("%s", qPrintable(errorMessage));
    m_d->m_firstActivatedFrame = false;
}

void CdbDebugEngine::selectThread(int index)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << index;

    //reset location arrow
    m_d->m_debuggerManager->resetLocation();

    ThreadsHandler *threadsHandler = m_d->m_debuggerManagerAccess->threadsHandler();
    threadsHandler->setCurrentThread(index);
    m_d->m_currentThreadId = index;
    m_d->updateStackTrace();
}

void CdbDebugEngine::attemptBreakpointSynchronization()
{
    QString errorMessage;
    if (!m_d->attemptBreakpointSynchronization(&errorMessage))
        qWarning("attemptBreakpointSynchronization: %s\n", qPrintable(errorMessage));
}

bool CdbDebugEnginePrivate::attemptBreakpointSynchronization(QString *errorMessage)
{
    if (!m_hDebuggeeProcess) {
        *errorMessage = QLatin1String("attemptBreakpointSynchronization() called while debugger is not running");
        return false;
    }
    // This is called from
    // 1) CreateProcessEvent with the halted engine
    // 2) from the break handler, potentially while the debuggee is running
    // If the debuggee is running (for which the execution status is
    // no reliable indicator), we temporarily halt and have ourselves
    // called again from the debug event handler.

    ULONG dummy;
    const bool wasRunning = !CDBBreakPoint::getBreakPointCount(m_pDebugControl, &dummy);
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << "\n  Running=" << wasRunning;

    if (wasRunning) {
        const HandleBreakEventMode oldMode = m_breakEventMode;
        m_breakEventMode = BreakEventSyncBreakPoints;
        if (!interruptInterferiorProcess(errorMessage)) {
            m_breakEventMode = oldMode;
            return false;
        }
        return true;
    }

    return CDBBreakPoint::synchronizeBreakPoints(m_pDebugControl,
                                                 m_debuggerManagerAccess->breakHandler(),
                                                 errorMessage);
}

void CdbDebugEngine::loadSessionData()
{
}

void CdbDebugEngine::saveSessionData()
{
}

void CdbDebugEngine::reloadDisassembler()
{
}

void CdbDebugEngine::reloadModules()
{
}

void CdbDebugEngine::loadSymbols(const QString &moduleName)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << moduleName;
}

void CdbDebugEngine::loadAllSymbols()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
}

static inline int registerFormatBase()
{
    switch(checkedRegisterFormatAction()) {
    case FormatHexadecimal:
        return 16;
    case FormatDecimal:
        return 10;
    case FormatOctal:
        return 8;
    case FormatBinary:
        return 2;
        break;
    case FormatRaw:
    case FormatNatural:
        break;
    }
    return 10;
}

void CdbDebugEngine::reloadRegisters()
{
    const int intBase = registerFormatBase();
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << intBase;
    QList<Register> registers;
    QString errorMessage;
    if (!getRegisters(m_d->m_pDebugControl, m_d->m_pDebugRegisters, &registers, &errorMessage, intBase))
        qWarning("reloadRegisters() failed: %s\n", qPrintable(errorMessage));
    m_d->m_debuggerManagerAccess->registerHandler()->setRegisters(registers);
}

void CdbDebugEngine::timerEvent(QTimerEvent* te)
{
    if (te->timerId() != m_d->m_watchTimer)
        return;

    const HRESULT hr = m_d->m_pDebugControl->WaitForEvent(0, 1);
    if (debugCDB)
        if (debugCDB > 1 || hr != S_FALSE)
            qDebug() << Q_FUNC_INFO << "WaitForEvent" << m_d->m_debuggerManager->status() <<   msgDebugEngineComResult(hr);

    switch (hr) {
        case S_OK:
            killWatchTimer();
            m_d->handleDebugEvent();
            break;
        case S_FALSE:
        case E_PENDING:
        case E_FAIL:
            break;
        case E_UNEXPECTED: // Occurs on ExitProcess.
            killWatchTimer();
            break;        
    }
}

void CdbDebugEngine::slotConsoleStubStarted()
{
    const qint64 appPid = m_d->m_consoleStubProc.applicationPID();
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << appPid;
    // Attach to console process
    QString errorMessage;
    if (startAttachDebugger(appPid, &errorMessage)) {
        m_d->m_debuggerManager->m_attachedPID = appPid;
        m_d->m_debuggerManagerAccess->notifyInferiorPidChanged(appPid);
    } else {
        QMessageBox::critical(m_d->m_debuggerManager->mainWindow(), tr("Debugger Error"), errorMessage);
    }
}

void CdbDebugEngine::slotConsoleStubError(const QString &msg)
{
    QMessageBox::critical(m_d->m_debuggerManager->mainWindow(), tr("Debugger Error"), msg);
}

void CdbDebugEngine::slotConsoleStubTerminated()
{
    exitDebugger();
}

void CdbDebugEnginePrivate::handleDebugEvent()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << m_hDebuggeeProcess;

    // restore mode and do special handling
    const HandleBreakEventMode mode = m_breakEventMode;
    m_breakEventMode = BreakEventHandle;

    switch (mode) {
    case BreakEventHandle:
        m_debuggerManagerAccess->notifyInferiorStopped();
        updateThreadList();
        updateStackTrace();
        break;
    case BreakEventIgnoreOnce:
        m_engine->startWatchTimer();
        break;
    case BreakEventSyncBreakPoints: {
            // Temp stop to sync breakpoints
            QString errorMessage;
            attemptBreakpointSynchronization(&errorMessage);
            m_engine->startWatchTimer();
            continueInferiorProcess(&errorMessage);
            if (!errorMessage.isEmpty())
                qWarning("handleDebugEvent: %s\n", qPrintable(errorMessage));
    }
        break;
    }
}

void CdbDebugEnginePrivate::setDebuggeeHandles(HANDLE hDebuggeeProcess,  HANDLE hDebuggeeThread)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << hDebuggeeProcess << hDebuggeeThread;
    m_hDebuggeeProcess = hDebuggeeProcess;
    m_hDebuggeeThread = hDebuggeeThread;
}

void CdbDebugEnginePrivate::updateThreadList()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << m_hDebuggeeProcess;

    ThreadsHandler* th = m_debuggerManagerAccess->threadsHandler();
    QList<ThreadData> threads;
    bool success = false;
    QString errorMessage;
    do {
        ULONG numberOfThreads;
        HRESULT hr= m_pDebugSystemObjects->GetNumberThreads(&numberOfThreads);
        if (FAILED(hr)) {
            errorMessage= msgComFailed("GetNumberThreads", hr);
            break;
        }
        const ULONG maxThreadIds = 256;
        ULONG threadIds[maxThreadIds];
        ULONG biggestThreadId = qMin(maxThreadIds, numberOfThreads - 1);
        hr = m_pDebugSystemObjects->GetThreadIdsByIndex(0, biggestThreadId, threadIds, 0);
        if (FAILED(hr)) {
            errorMessage= msgComFailed("GetThreadIdsByIndex", hr);
            break;
        }
        for (ULONG threadId = 0; threadId <= biggestThreadId; ++threadId) {
            ThreadData thread;
            thread.id = threadId;
            threads.append(thread);
        }

        th->setThreads(threads);
        success = true;
    } while (false);
    if (!success)
        qWarning("updateThreadList() failed: %s\n", qPrintable(errorMessage));
}

void CdbDebugEnginePrivate::updateStackTrace()
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
    // Create a new context
    clearForRun();
    QString errorMessage;
    m_engine->reloadRegisters();
    m_currentStackTrace =
            CdbStackTraceContext::create(m_pDebugControl, m_pDebugSystemObjects,
                                         m_pDebugSymbols, m_currentThreadId, &errorMessage);
    if (!m_currentStackTrace) {
        qWarning("%s: failed to create trace context: %s", Q_FUNC_INFO, qPrintable(errorMessage));
        return;
    }
    const QList<StackFrame> stackFrames = m_currentStackTrace->frames();
    // find the first usable frame and select it
    int current = -1;
    const int count = stackFrames.count();
    for (int i=0; i < count; ++i)
        if (stackFrames.at(i).isUsable()) {
            current = i;
            break;
        }

    m_debuggerManagerAccess->stackHandler()->setFrames(stackFrames);
    m_firstActivatedFrame = true;
    if (current >= 0) {
        m_debuggerManagerAccess->stackHandler()->setCurrentIndex(current);
        m_engine->activateFrame(current);
    }    
}


void CdbDebugEnginePrivate::updateModules()
{
    QList<Module> modules;
    QString errorMessage;
    if (!getModuleList(m_pDebugSymbols, &modules, &errorMessage))
        qWarning("updateModules() failed: %s\n", qPrintable(errorMessage));
    m_debuggerManagerAccess->modulesHandler()->setModules(modules);
}

void CdbDebugEnginePrivate::handleBreakpointEvent(PDEBUG_BREAKPOINT pBP)
{
    Q_UNUSED(pBP)
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
}

void CdbDebugEngine::reloadSourceFiles()
{
}

} // namespace Internal
} // namespace Debugger

// Accessed by DebuggerManager
Debugger::Internal::IDebuggerEngine *createWinEngine(Debugger::Internal::DebuggerManager *parent)
{
    return Debugger::Internal::CdbDebugEngine::create(parent);
}

