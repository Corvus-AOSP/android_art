/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !defined(ART_USE_LLVM_COMPILER)
#include "callee_save_frame.h"
#endif
#include "dex_instruction.h"
#include "object.h"
#include "object_utils.h"
#if defined(ART_USE_LLVM_COMPILER)
#include "nth_caller_visitor.h"
#endif
#include "scoped_thread_state_change.h"

// Architecture specific assembler helper to deliver exception.
extern "C" void art_deliver_exception_from_code(void*);

namespace art {

#if !defined(ART_USE_LLVM_COMPILER)
// Lazily resolve a method. Called by stub code.
const void* UnresolvedDirectMethodTrampolineFromCode(AbstractMethod* called, AbstractMethod** sp, Thread* thread,
                                                     Runtime::TrampolineType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__arm__)
  // On entry the stack pointed by sp is:
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | ...        |    callee saves
  // | R3         |    arg3
  // | R2         |    arg2
  // | R1         |    arg1
  // | R0         |
  // | Method*    |  <- sp
  DCHECK_EQ(48U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  AbstractMethod** caller_sp = reinterpret_cast<AbstractMethod**>(reinterpret_cast<byte*>(sp) + 48);
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp) + kPointerSize);
  uintptr_t caller_pc = regs[10];
#elif defined(__i386__)
  // On entry the stack pointed by sp is:
  // | argN        |  |
  // | ...         |  |
  // | arg4        |  |
  // | arg3 spill  |  |  Caller's frame
  // | arg2 spill  |  |
  // | arg1 spill  |  |
  // | Method*     | ---
  // | Return      |
  // | EBP,ESI,EDI |    callee saves
  // | EBX         |    arg3
  // | EDX         |    arg2
  // | ECX         |    arg1
  // | EAX/Method* |  <- sp
  DCHECK_EQ(32U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  AbstractMethod** caller_sp = reinterpret_cast<AbstractMethod**>(reinterpret_cast<byte*>(sp) + 32);
  uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp));
  uintptr_t caller_pc = regs[7];
#else
  UNIMPLEMENTED(FATAL);
  AbstractMethod** caller_sp = NULL;
  uintptr_t* regs = NULL;
  uintptr_t caller_pc = 0;
#endif
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kRefsAndArgs);
  // Start new JNI local reference state
  JNIEnvExt* env = thread->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  AbstractMethod* caller = *caller_sp;
  InvokeType invoke_type;
  uint32_t dex_method_idx;
#if !defined(__i386__)
  const char* shorty;
  uint32_t shorty_len;
#endif
  if (type == Runtime::kUnknownMethod) {
    DCHECK(called->IsRuntimeMethod());
    uint32_t dex_pc = caller->ToDexPc(caller_pc);
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:  // Fall-through.
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        break;
      case Instruction::INVOKE_STATIC:  // Fall-through.
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        break;
      case Instruction::INVOKE_SUPER:  // Fall-through.
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        break;
      case Instruction::INVOKE_VIRTUAL:  // Fall-through.
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(NULL);
        invoke_type = kDirect;  // Avoid used uninitialized warnings.
    }
    DecodedInstruction dec_insn(instr);
    dex_method_idx = dec_insn.vB;
#if !defined(__i386__)
    shorty = linker->MethodShorty(dex_method_idx, caller, &shorty_len);
#endif
  } else {
    DCHECK(!called->IsRuntimeMethod());
    invoke_type = (type == Runtime::kStaticMethod) ? kStatic : kDirect;
    dex_method_idx = called->GetDexMethodIndex();
#if !defined(__i386__)
    MethodHelper mh(called);
    shorty = mh.GetShorty();
    shorty_len = mh.GetShortyLength();
#endif
  }
#if !defined(__i386__)
  // Discover shorty (avoid GCs)
  size_t args_in_regs = 0;
  for (size_t i = 1; i < shorty_len; i++) {
    char c = shorty[i];
    args_in_regs = args_in_regs + (c == 'J' || c == 'D' ? 2 : 1);
    if (args_in_regs > 3) {
      args_in_regs = 3;
      break;
    }
  }
  // Place into local references incoming arguments from the caller's register arguments
  size_t cur_arg = 1;   // skip method_idx in R0, first arg is in R1
  if (invoke_type != kStatic) {
    Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
    cur_arg++;
    if (args_in_regs < 3) {
      // If we thought we had fewer than 3 arguments in registers, account for the receiver
      args_in_regs++;
    }
    soa.AddLocalReference<jobject>(obj);
  }
  size_t shorty_index = 1;  // skip return value
  // Iterate while arguments and arguments in registers (less 1 from cur_arg which is offset to skip
  // R0)
  while ((cur_arg - 1) < args_in_regs && shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
      soa.AddLocalReference<jobject>(obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
  // Place into local references incoming arguments from the caller's stack arguments
  cur_arg += 11;  // skip LR, Method* and spills for R1 to R3 and callee saves
  while (shorty_index < shorty_len) {
    char c = shorty[shorty_index];
    shorty_index++;
    if (c == 'L') {
      Object* obj = reinterpret_cast<Object*>(regs[cur_arg]);
      soa.AddLocalReference<jobject>(obj);
    }
    cur_arg = cur_arg + (c == 'J' || c == 'D' ? 2 : 1);
  }
#endif
  // Resolve method filling in dex cache
  if (type == Runtime::kUnknownMethod) {
    called = linker->ResolveMethod(dex_method_idx, caller, invoke_type);
  }
  const void* code = NULL;
  if (LIKELY(!thread->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type));
    // Ensure that the called method's class is initialized.
    Class* called_class = called->GetDeclaringClass();
    linker->EnsureInitialized(called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      code = called->GetCode();
    } else if (called_class->IsInitializing()) {
      if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetCode();
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  if (UNLIKELY(code == NULL)) {
    // Something went wrong in ResolveMethod or EnsureInitialized,
    // go into deliver exception with the pending exception in r0
    code = reinterpret_cast<void*>(art_deliver_exception_from_code);
    regs[0] = reinterpret_cast<uintptr_t>(thread->GetException());
    thread->ClearException();
  } else {
    // Expect class to at least be initializing.
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Don't want infinite recursion.
    DCHECK(code != Runtime::Current()->GetResolutionStubArray(Runtime::kUnknownMethod)->GetData());
    // Set up entry into main method
    regs[0] = reinterpret_cast<uintptr_t>(called);
  }
  return code;
}
#else // ART_USE_LLVM_COMPILER
const void* UnresolvedDirectMethodTrampolineFromCode(AbstractMethod* called, AbstractMethod** called_addr,
                                                     Thread* thread, Runtime::TrampolineType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t dex_pc;
  AbstractMethod* caller = thread->GetCurrentMethod(&dex_pc);

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  InvokeType invoke_type;
  uint32_t dex_method_idx;
  if (type == Runtime::kUnknownMethod) {
    DCHECK(called->IsRuntimeMethod());
    const DexFile::CodeItem* code = MethodHelper(caller).GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:  // Fall-through.
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        break;
      case Instruction::INVOKE_STATIC:  // Fall-through.
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        break;
      case Instruction::INVOKE_SUPER:  // Fall-through.
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        break;
      case Instruction::INVOKE_VIRTUAL:  // Fall-through.
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(NULL);
        invoke_type = kDirect;  // Avoid used uninitialized warnings.
    }
    DecodedInstruction dec_insn(instr);
    dex_method_idx = dec_insn.vB;
  } else {
    DCHECK(!called->IsRuntimeMethod());
    invoke_type = (type == Runtime::kStaticMethod) ? kStatic : kDirect;
    dex_method_idx = called->GetDexMethodIndex();
  }
  if (type == Runtime::kUnknownMethod) {
    called = linker->ResolveMethod(dex_method_idx, caller, invoke_type);
  }
  const void* code = NULL;
  if (LIKELY(!thread->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type));
    // Ensure that the called method's class is initialized.
    Class* called_class = called->GetDeclaringClass();
    linker->EnsureInitialized(called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      code = called->GetCode();
      // TODO: remove this after we solve the link issue.
      { // for lazy link.
        if (code == NULL) {
          code = linker->GetOatCodeFor(called);
        }
      }
    } else if (called_class->IsInitializing()) {
      if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetCode();
        // TODO: remove this after we solve the link issue.
        { // for lazy link.
          if (code == NULL) {
            code = linker->GetOatCodeFor(called);
          }
        }
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  if (LIKELY(code != NULL)) {
    // Expect class to at least be initializing.
    DCHECK(called->GetDeclaringClass()->IsInitializing());
    // Don't want infinite recursion.
    DCHECK(code != Runtime::Current()->GetResolutionStubArray(Runtime::kUnknownMethod)->GetData());
    // Set up entry into main method
    *called_addr = called;
  }
  return code;
}
#endif // ART_USE_LLVM_COMPILER

#if !defined(ART_USE_LLVM_COMPILER)
// Called by the AbstractMethodError. Called by stub code.
extern void ThrowAbstractMethodErrorFromCode(AbstractMethod* method, Thread* thread, AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
  thread->DeliverException();
}
#else // ART_USE_LLVM_COMPILER
extern void ThrowAbstractMethodErrorFromCode(AbstractMethod* method, Thread* thread, AbstractMethod**)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;",
                             "abstract method \"%s\"", PrettyMethod(method).c_str());
}
#endif // ART_USE_LLVM_COMPILER

}  // namespace art
