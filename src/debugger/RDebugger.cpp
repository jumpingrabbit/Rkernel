//  Rkernel is an execution kernel for R interpreter
//  Copyright (C) 2019 JetBrains s.r.o.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "../RPIServiceImpl.h"
#include "RDebugger.h"
#include "SourceFileManager.h"
#include "../RStuff/RUtil.h"
#include "../RInternals/RInternals.h"
#include "../RStuff/Export.h"

RDebugger rDebugger;

static SEXP debugDoBegin(SEXP call, SEXP op, SEXP args, SEXP rho);

void RDebugger::init() {
  runToPositionTarget = R_NilValue;
}

void RDebugger::enable() {
  if (_isEnabled) return;
  _isEnabled = true;
  prevJIT = asInt(RI->compilerEnableJIT(0));
  int beginOffset = getPrimOffset(RI->begin);
  prevDoBegin = getFunTabFunction(beginOffset);
  setFunTabFunction(beginOffset, debugDoBegin);
}

void RDebugger::disable() {
  if (!_isEnabled) return;
  _isEnabled = false;
  int beginOffset = getPrimOffset(RI->begin);
  setFunTabFunction(beginOffset, prevDoBegin);
  RI->compilerEnableJIT(prevJIT);
}

bool RDebugger::isEnabled() {
  return _isEnabled;
}

void RDebugger::clearStack() {
  stack.clear();
}

void RDebugger::muteBreakpoints(bool mute) {
  breakpointsMuted = mute;
}

static void setBreakpointInfoAttrib(SEXP srcref, BreakpointInfo* info) {
  if (info == nullptr) {
    Rf_setAttrib(srcref, RI->breakpointInfoAttr, R_NilValue);
    return;
  }
  Rf_setAttrib(srcref, RI->breakpointInfoAttr, R_MakeExternalPtr(info, R_NilValue, R_NilValue));
}

static BreakpointInfo* getBreakpointInfoAttrib(SEXP srcref) {
  SEXP attr = Rf_getAttrib(srcref, RI->breakpointInfoAttr);
  if (TYPEOF(attr) != EXTPTRSXP) {
    return nullptr;
  }
  return (BreakpointInfo*)EXTPTR_PTR(attr);
}

BreakpointInfo& RDebugger::addBreakpoint(std::string const& file, int line) {
  auto result = breakpoints[file].insert({line, InternalBreakpointInfo()});
  auto it = result.first;
  auto inserted = result.second;
  if (inserted) {
    SEXP srcref = sourceFileManager.getStepSrcref(file, line);
    it->second.srcref = srcref;
    if (srcref != R_NilValue) {
      SET_RDEBUG(srcref, true);
      setBreakpointInfoAttrib(srcref, &it->second.info);
    }
  }
  return it->second.info;
}

void RDebugger::removeBreakpoint(std::string const& file, int line) {
  auto it = breakpoints.find(file);
  if (it != breakpoints.end()) {
    auto it2 = it->second.find(line);
    if (it2 != it->second.end()) {
      setBreakpointInfoAttrib(it2->second.srcref, nullptr);
      SET_RDEBUG(it2->second.srcref, false);
      it->second.erase(it2);
    }
  }
}

void RDebugger::refreshBreakpoint(std::string const& file, int line) {
  auto it = breakpoints.find(file);
  if (it != breakpoints.end()) {
    auto it2 = it->second.find(line);
    if (it2 != it->second.end()) {
      setBreakpointInfoAttrib(it2->second.srcref, nullptr);
      SET_RDEBUG(it2->second.srcref, false);
      SEXP srcref = sourceFileManager.getStepSrcref(file, line);
      it2->second.srcref = srcref;
      if (srcref != R_NilValue) {
        SET_RDEBUG(srcref, true);
        setBreakpointInfoAttrib(srcref, &it2->second.info);
      }
    }
  }
}

void RDebugger::setCommand(DebuggerCommand c) {
  currentCommand = c;
  resetRunToPositionTarget();
  runToPositionTarget = R_NilValue;
  RContext* ctx = getGlobalContext();
  bool isFirst = true;
  while (ctx != nullptr) {
    if (isCallContext(ctx)) {
      SEXP env = getEnvironment(ctx);
      switch (c) {
        case CONTINUE:
        case STEP_INTO: {
          Rf_setAttrib(env, RI->stopHereFlagAttr, R_NilValue);
          break;
        }
        case STEP_OVER: {
          Rf_setAttrib(env, RI->stopHereFlagAttr, toSEXP(true));
          break;
        }
        case STEP_OUT: {
          Rf_setAttrib(env, RI->stopHereFlagAttr, isFirst ? R_NilValue : toSEXP(true));
          break;
        }
        default:;
      }
      isFirst = false;
    }
    ctx = getNextContext(ctx);
  }
}

void RDebugger::setRunToPositionCommand(std::string const& fileId, int line) {
  currentCommand = CONTINUE;
  resetRunToPositionTarget();
  ShieldSEXP srcref = sourceFileManager.getStepSrcref(fileId, line);
  if (srcref != R_NilValue) {
    runToPositionTarget = srcref;
    SET_RDEBUG(srcref, true);
  }
}

void RDebugger::resetRunToPositionTarget() {
  if (runToPositionTarget != R_NilValue) {
    SET_RDEBUG(runToPositionTarget, false);
    auto position = srcrefToPosition(runToPositionTarget);
    refreshBreakpoint(position.first, position.second);
    runToPositionTarget = R_NilValue;
  }
}

static bool checkCondition(std::string const& condition, ShieldSEXP const& env) {
  if (condition.empty()) {
    return true;
  }
  try {
    WithDebuggerEnabled with(false);
    ShieldSEXP expr = parseCode(condition);
    ShieldSEXP result = RI->asLogical(RI->evalq(expr, env));
    return asBool(result);
  } catch (RError const&) {
    return false;
  }
}

static void evaluateAndLog(std::string const& expression, ShieldSEXP const& env) {
  if (expression.empty()) {
    return;
  }
  try {
    WithDebuggerEnabled with(false);
    ShieldSEXP expr = parseCode(expression);
    RI->message(getPrintedValue(RI->evalq(expr, env)), named("appendLF", false));
  } catch (RError const& e) {
    RI->message(e.what());
  }
}

void RDebugger::doBreakpoint(SEXP currentCall, BreakpointInfo const* breakpoint, bool isStepStop, SEXP env) {
  if (!isEnabled() || R_interrupts_pending) return;

  if (currentCommand == STOP) {
    setCommand(CONTINUE);
    R_interrupts_pending = 1;
    R_CheckUserInterrupt();
    return;
  }

  CPP_BEGIN
    bool suspend = isStepStop || (R_Srcref != R_NilValue && R_Srcref == runToPositionTarget);
    if (!breakpointsMuted && breakpoint != nullptr) {
      if (checkCondition(breakpoint->condition, env)) {
        evaluateAndLog(breakpoint->evaluateAndLog, env);
        if (breakpoint->suspend) {
          suspend = true;
        }
      }
    }

    if (!suspend) return;
    setCommand(CONTINUE);
    stack = buildStack(getContextDump(currentCall));

    rpiService->debugPromptHandler();
  CPP_END_VOID
}

void RDebugger::buildDebugPrompt(AsyncEvent::DebugPrompt* prompt) {
  prompt->set_changed(true);
  buildStackProto(stack, prompt->mutable_stack());
}

static SEXP debugDoBegin(SEXP call, SEXP, SEXP args, SEXP rho) {
  return rDebugger.doBegin(call, args, rho);
}

static RContext* getCurrentCallContext() {
  RContext* ctx = getGlobalContext();
  while (ctx != nullptr && !isCallContext(ctx)) {
    ctx = getNextContext(ctx);
  }
  return ctx;
}

SEXP RDebugger::doBegin(SEXP call, SEXP args, SEXP rho) {
  SEXP s = R_NilValue;
  RContext* ctx = getCurrentCallContext();
  SEXP function = R_NilValue, functionEnv = R_NilValue;
  const char* suggestedFunctionName = "";
  if (ctx != nullptr) {
    function = getFunction(ctx);
    functionEnv = getEnvironment(ctx);
    suggestedFunctionName = getCallFunctionName(getCall(ctx));
  }
  sourceFileManager.getFunctionSrcref(function, suggestedFunctionName);
  SEXP srcrefs = getBlockSrcrefs(call);
  bool isPhysical;
  {
    PROTECT(R_Srcref = getSrcref(srcrefs, 0));
    SEXP srcfile = Rf_getAttrib(R_Srcref, RI->srcfileAttr);
    isPhysical = Rf_getAttrib(srcfile, RI->isPhysicalFileFlag) != R_NilValue;
    if (RDEBUG(R_Srcref)) {
      doBreakpoint(CAR(call), getBreakpointInfoAttrib(R_Srcref), false, rho);
    }
    UNPROTECT(1);
  }
  if (args != R_NilValue) {
    PROTECT(srcrefs);
    int i = 1;
    while (args != R_NilValue) {
      PROTECT(R_Srcref = getSrcref(srcrefs, i++));
      bool stopHere;
      switch (currentCommand) {
        case STEP_INTO: {
          stopHere = isPhysical;
          break;
        }
        case FORCE_STEP_INTO:
        case PAUSE:
        case STOP: {
          stopHere = true;
          break;
        }
        case STEP_OVER:
        case STEP_OUT: {
          stopHere = Rf_getAttrib(functionEnv, RI->stopHereFlagAttr) != R_NilValue;
          break;
        }
        default: {
          stopHere = false;
          break;
        }
      }
      bool rDebugFlag = RDEBUG(R_Srcref);
      if (stopHere || rDebugFlag) {
        doBreakpoint(CAR(args), rDebugFlag ? getBreakpointInfoAttrib(R_Srcref) : nullptr, stopHere, rho);
      }
      s = Rf_eval(CAR(args), rho);
      UNPROTECT(1);
      args = CDR(args);
    }
    R_Srcref = R_NilValue;
    UNPROTECT(1);
  }
  return s;
}

void RDebugger::doHandleException(ShieldSEXP const& e) {
  lastErrorStackDump = getContextDump(R_NilValue);
  lastError = std::make_unique<PrSEXP>(e);
}

std::vector<RDebugger::ContextDump> RDebugger::getContextDump(ShieldSEXP const& currentCall) {
  std::vector<ContextDump> dump;
  ContextDump initial { currentCall, R_NilValue, R_Srcref ? R_Srcref : R_NilValue, R_NilValue };
  dump.push_back(initial);
  RContext* ctx = getGlobalContext();
  while (ctx != nullptr) {
    if (isCallContext(ctx)) {
      SEXP srcref = getSrcref(ctx);
      ContextDump currentContext { getCall(ctx), getFunction(ctx), srcref ? srcref : R_NilValue, getEnvironment(ctx) };
      dump.push_back(currentContext);
    }
    ctx = getNextContext(ctx);
  }
  std::reverse(dump.begin(), dump.end());
  return dump;
}

std::vector<RDebuggerStackFrame> RDebugger::buildStack(std::vector<ContextDump> const& contexts) {
  std::vector<RDebuggerStackFrame> stack;
  if (contexts.empty()) return stack;
  WithDebuggerEnabled with(false);

  bool wasStackBottom = false;
  std::string functionName;
  SEXP frame = R_NilValue;
  SEXP functionSrcref = R_NilValue;
  for (auto const& ctx : contexts) {
    SEXP call = ctx.call;
    SEXP srcref = ctx.srcref;
    srcref = (srcref == nullptr) ? R_NilValue : srcref;
    if (srcref == R_NilValue) {
      srcref = Rf_getAttrib(call, RI->srcrefAttr);
      if (srcref == R_NilValue) {
        srcref = functionSrcref;
      }
    }
    SEXP srcfile = Rf_getAttrib(srcref, RI->srcfileAttr);
    if (Rf_getAttrib(frame, RI->stackBottomAttr) != R_NilValue && ctx.environment != R_NilValue) {
      stack.clear();
      wasStackBottom = true;
    } else {
      wasStackBottom = wasStackBottom || Rf_getAttrib(srcfile, RI->isPhysicalFileFlag) != R_NilValue;
      if (call != R_NilValue && wasStackBottom) {
        auto position = srcrefToPosition(srcref);
        SEXP realFrame = Rf_getAttrib(frame, RI->realEnvAttr);
        if (realFrame != R_NilValue) {
          frame = realFrame;
        }
        if (stack.empty()) functionName = "";
        stack.push_back({position.first, position.second, frame, functionName});
      }
    }
    functionName = getCallFunctionName(call);
    if (ctx.function != R_NilValue) {
      functionSrcref = sourceFileManager.getFunctionSrcref((SEXP)ctx.function, functionName);
    }
    frame = ctx.environment;
  }
  return stack;
}

void buildStackProto(std::vector<RDebuggerStackFrame> const& stack, StackFrameList *listProto) {
  for (auto const& frame : stack) {
    auto proto = listProto->add_frames();
    proto->mutable_position()->set_fileid(frame.fileId);
    proto->mutable_position()->set_line(frame.line);
    proto->set_functionname(frame.functionName);
    proto->set_equalityobject((long long)(SEXP)frame.environment);
  }
}

std::vector<RDebuggerStackFrame> const& RDebugger::getStack() {
  return stack;
}

std::vector<RDebuggerStackFrame> RDebugger::getLastErrorStack() {
  std::vector<RDebuggerStackFrame> result = buildStack(lastErrorStackDump);
  if (!result.empty()) result.pop_back();
  return result;
}

void RDebugger::resetLastErrorStack() {
  lastErrorStackDump.clear();
}
