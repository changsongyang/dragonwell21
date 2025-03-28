/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "code/compiledIC.hpp"
#include "code/compiledMethod.inline.hpp"
#include "code/exceptionHandlerTable.hpp"
#include "code/scopeDesc.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "interpreter/bytecode.inline.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "memory/resourceArea.hpp"
#include "oops/compiledICHolder.inline.hpp"
#include "oops/klass.inline.hpp"
#include "oops/methodData.hpp"
#include "oops/method.inline.hpp"
#include "oops/weakHandle.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/atomic.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/sharedRuntime.hpp"

CompiledMethod::CompiledMethod(Method* method, const char* name, CompilerType type, int size,
                               int header_size, CodeBuffer* cb, int frame_complete_offset, int frame_size,
                               OopMapSet* oop_maps, bool caller_must_gc_arguments)
  : CodeBlob(name, CodeBlobKind::Nmethod, type, cb, size, header_size,
             frame_complete_offset, frame_size, oop_maps, caller_must_gc_arguments),
    _deoptimization_status(not_marked),
    _deoptimization_generation(0),
    _method(method),
    _gc_data(nullptr)
{
  init_defaults();
}

void CompiledMethod::init_defaults() {
  { // avoid uninitialized fields, even for short time periods
    _scopes_data_begin          = nullptr;
    _deopt_handler_begin        = nullptr;
    _deopt_mh_handler_begin     = nullptr;
    _exception_cache            = nullptr;
  }
  _has_unsafe_access          = 0;
  _has_method_handle_invokes  = 0;
  _has_wide_vectors           = 0;
  _has_monitors               = 0;
}

bool CompiledMethod::is_method_handle_return(address return_pc) {
  if (!has_method_handle_invokes())  return false;
  PcDesc* pd = pc_desc_at(return_pc);
  if (pd == nullptr)
    return false;
  return pd->is_method_handle_invoke();
}

// Returns a string version of the method state.
const char* CompiledMethod::state() const {
  int state = get_state();
  switch (state) {
  case not_installed:
    return "not installed";
  case in_use:
    return "in use";
  case not_used:
    return "not_used";
  case not_entrant:
    return "not_entrant";
  default:
    fatal("unexpected method state: %d", state);
    return nullptr;
  }
}

//-----------------------------------------------------------------------------
void CompiledMethod::set_deoptimized_done() {
  MutexLocker ml(CompiledMethod_lock->owned_by_self() ? nullptr : CompiledMethod_lock, Mutex::_no_safepoint_check_flag);
  if (_deoptimization_status != deoptimize_done) { // can't go backwards
    Atomic::store(&_deoptimization_status, deoptimize_done);
  }
}

//-----------------------------------------------------------------------------

ExceptionCache* CompiledMethod::exception_cache_acquire() const {
  return Atomic::load_acquire(&_exception_cache);
}

void CompiledMethod::add_exception_cache_entry(ExceptionCache* new_entry) {
  assert(ExceptionCache_lock->owned_by_self(),"Must hold the ExceptionCache_lock");
  assert(new_entry != nullptr,"Must be non null");
  assert(new_entry->next() == nullptr, "Must be null");

  for (;;) {
    ExceptionCache *ec = exception_cache();
    if (ec != nullptr) {
      Klass* ex_klass = ec->exception_type();
      if (!ex_klass->is_loader_alive()) {
        // We must guarantee that entries are not inserted with new next pointer
        // edges to ExceptionCache entries with dead klasses, due to bad interactions
        // with concurrent ExceptionCache cleanup. Therefore, the inserts roll
        // the head pointer forward to the first live ExceptionCache, so that the new
        // next pointers always point at live ExceptionCaches, that are not removed due
        // to concurrent ExceptionCache cleanup.
        ExceptionCache* next = ec->next();
        if (Atomic::cmpxchg(&_exception_cache, ec, next) == ec) {
          CodeCache::release_exception_cache(ec);
        }
        continue;
      }
      ec = exception_cache();
      if (ec != nullptr) {
        new_entry->set_next(ec);
      }
    }
    if (Atomic::cmpxchg(&_exception_cache, ec, new_entry) == ec) {
      return;
    }
  }
}

void CompiledMethod::clean_exception_cache() {
  // For each nmethod, only a single thread may call this cleanup function
  // at the same time, whether called in STW cleanup or concurrent cleanup.
  // Note that if the GC is processing exception cache cleaning in a concurrent phase,
  // then a single writer may contend with cleaning up the head pointer to the
  // first ExceptionCache node that has a Klass* that is alive. That is fine,
  // as long as there is no concurrent cleanup of next pointers from concurrent writers.
  // And the concurrent writers do not clean up next pointers, only the head.
  // Also note that concurrent readers will walk through Klass* pointers that are not
  // alive. That does not cause ABA problems, because Klass* is deleted after
  // a handshake with all threads, after all stale ExceptionCaches have been
  // unlinked. That is also when the CodeCache::exception_cache_purge_list()
  // is deleted, with all ExceptionCache entries that were cleaned concurrently.
  // That similarly implies that CAS operations on ExceptionCache entries do not
  // suffer from ABA problems as unlinking and deletion is separated by a global
  // handshake operation.
  ExceptionCache* prev = nullptr;
  ExceptionCache* curr = exception_cache_acquire();

  while (curr != nullptr) {
    ExceptionCache* next = curr->next();

    if (!curr->exception_type()->is_loader_alive()) {
      if (prev == nullptr) {
        // Try to clean head; this is contended by concurrent inserts, that
        // both lazily clean the head, and insert entries at the head. If
        // the CAS fails, the operation is restarted.
        if (Atomic::cmpxchg(&_exception_cache, curr, next) != curr) {
          prev = nullptr;
          curr = exception_cache_acquire();
          continue;
        }
      } else {
        // It is impossible to during cleanup connect the next pointer to
        // an ExceptionCache that has not been published before a safepoint
        // prior to the cleanup. Therefore, release is not required.
        prev->set_next(next);
      }
      // prev stays the same.

      CodeCache::release_exception_cache(curr);
    } else {
      prev = curr;
    }

    curr = next;
  }
}

// public method for accessing the exception cache
// These are the public access methods.
address CompiledMethod::handler_for_exception_and_pc(Handle exception, address pc) {
  // We never grab a lock to read the exception cache, so we may
  // have false negatives. This is okay, as it can only happen during
  // the first few exception lookups for a given nmethod.
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != nullptr) {
    address ret_val;
    if ((ret_val = ec->match(exception,pc)) != nullptr) {
      return ret_val;
    }
    ec = ec->next();
  }
  return nullptr;
}

void CompiledMethod::add_handler_for_exception_and_pc(Handle exception, address pc, address handler) {
  // There are potential race conditions during exception cache updates, so we
  // must own the ExceptionCache_lock before doing ANY modifications. Because
  // we don't lock during reads, it is possible to have several threads attempt
  // to update the cache with the same data. We need to check for already inserted
  // copies of the current data before adding it.

  MutexLocker ml(ExceptionCache_lock);
  ExceptionCache* target_entry = exception_cache_entry_for_exception(exception);

  if (target_entry == nullptr || !target_entry->add_address_and_handler(pc,handler)) {
    target_entry = new ExceptionCache(exception,pc,handler);
    add_exception_cache_entry(target_entry);
  }
}

// private method for handling exception cache
// These methods are private, and used to manipulate the exception cache
// directly.
ExceptionCache* CompiledMethod::exception_cache_entry_for_exception(Handle exception) {
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != nullptr) {
    if (ec->match_exception_with_space(exception)) {
      return ec;
    }
    ec = ec->next();
  }
  return nullptr;
}

//-------------end of code for ExceptionCache--------------

bool CompiledMethod::is_at_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    if (iter.type() == relocInfo::poll_return_type)
      return true;
  }
  return false;
}


bool CompiledMethod::is_at_poll_or_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    relocInfo::relocType t = iter.type();
    if (t == relocInfo::poll_return_type || t == relocInfo::poll_type)
      return true;
  }
  return false;
}

void CompiledMethod::verify_oop_relocations() {
  // Ensure sure that the code matches the current oop values
  RelocIterator iter(this, nullptr, nullptr);
  while (iter.next()) {
    if (iter.type() == relocInfo::oop_type) {
      oop_Relocation* reloc = iter.oop_reloc();
      if (!reloc->oop_is_immediate()) {
        reloc->verify_oop_relocation();
      }
    }
  }
}


ScopeDesc* CompiledMethod::scope_desc_at(address pc) {
  PcDesc* pd = pc_desc_at(pc);
  guarantee(pd != nullptr, "scope must be present");
  return new ScopeDesc(this, pd);
}

ScopeDesc* CompiledMethod::scope_desc_near(address pc) {
  PcDesc* pd = pc_desc_near(pc);
  guarantee(pd != nullptr, "scope must be present");
  return new ScopeDesc(this, pd);
}

address CompiledMethod::oops_reloc_begin() const {
  // If the method is not entrant then a JMP is plastered over the
  // first few bytes.  If an oop in the old code was there, that oop
  // should not get GC'd.  Skip the first few bytes of oops on
  // not-entrant methods.
  if (frame_complete_offset() != CodeOffsets::frame_never_safe &&
      code_begin() + frame_complete_offset() >
      verified_entry_point() + NativeJump::instruction_size)
  {
    // If we have a frame_complete_offset after the native jump, then there
    // is no point trying to look for oops before that. This is a requirement
    // for being allowed to scan oops concurrently.
    return code_begin() + frame_complete_offset();
  }

  // It is not safe to read oops concurrently using entry barriers, if their
  // location depend on whether the nmethod is entrant or not.
  // assert(BarrierSet::barrier_set()->barrier_set_nmethod() == nullptr, "Not safe oop scan");

  address low_boundary = verified_entry_point();
  if (!is_in_use() && is_nmethod()) {
    low_boundary += NativeJump::instruction_size;
    // %%% Note:  On SPARC we patch only a 4-byte trap, not a full NativeJump.
    // This means that the low_boundary is going to be a little too high.
    // This shouldn't matter, since oops of non-entrant methods are never used.
    // In fact, why are we bothering to look at oops in a non-entrant method??
  }
  return low_boundary;
}

int CompiledMethod::verify_icholder_relocations() {
  ResourceMark rm;
  int count = 0;

  RelocIterator iter(this);
  while(iter.next()) {
    if (iter.type() == relocInfo::virtual_call_type) {
      if (CompiledIC::is_icholder_call_site(iter.virtual_call_reloc(), this)) {
        CompiledIC *ic = CompiledIC_at(&iter);
        if (TraceCompiledIC) {
          tty->print("noticed icholder " INTPTR_FORMAT " ", p2i(ic->cached_icholder()));
          ic->print();
        }
        assert(ic->cached_icholder() != nullptr, "must be non-nullptr");
        count++;
      }
    }
  }

  return count;
}

// Method that knows how to preserve outgoing arguments at call. This method must be
// called with a frame corresponding to a Java invoke
void CompiledMethod::preserve_callee_argument_oops(frame fr, const RegisterMap *reg_map, OopClosure* f) {
  if (method() == nullptr) {
    return;
  }

  // handle the case of an anchor explicitly set in continuation code that doesn't have a callee
  JavaThread* thread = reg_map->thread();
  if (thread->has_last_Java_frame() && fr.sp() == thread->last_Java_sp()) {
    return;
  }

  if (!method()->is_native()) {
    address pc = fr.pc();
    bool has_receiver, has_appendix;
    Symbol* signature;

    // The method attached by JIT-compilers should be used, if present.
    // Bytecode can be inaccurate in such case.
    Method* callee = attached_method_before_pc(pc);
    if (callee != nullptr) {
      has_receiver = !(callee->access_flags().is_static());
      has_appendix = false;
      signature    = callee->signature();
    } else {
      SimpleScopeDesc ssd(this, pc);

      Bytecode_invoke call(methodHandle(Thread::current(), ssd.method()), ssd.bci());
      has_receiver = call.has_receiver();
      has_appendix = call.has_appendix();
      signature    = call.signature();
    }

    fr.oops_compiled_arguments_do(signature, has_receiver, has_appendix, reg_map, f);
  } else if (method()->is_continuation_enter_intrinsic()) {
    // This method only calls Continuation.enter()
    Symbol* signature = vmSymbols::continuationEnter_signature();
    fr.oops_compiled_arguments_do(signature, false, false, reg_map, f);
  }
}

Method* CompiledMethod::attached_method(address call_instr) {
  assert(code_contains(call_instr), "not part of the nmethod");
  RelocIterator iter(this, call_instr, call_instr + 1);
  while (iter.next()) {
    if (iter.addr() == call_instr) {
      switch(iter.type()) {
        case relocInfo::static_call_type:      return iter.static_call_reloc()->method_value();
        case relocInfo::opt_virtual_call_type: return iter.opt_virtual_call_reloc()->method_value();
        case relocInfo::virtual_call_type:     return iter.virtual_call_reloc()->method_value();
        default:                               break;
      }
    }
  }
  return nullptr; // not found
}

Method* CompiledMethod::attached_method_before_pc(address pc) {
  if (NativeCall::is_call_before(pc)) {
    NativeCall* ncall = nativeCall_before(pc);
    return attached_method(ncall->instruction_address());
  }
  return nullptr; // not a call
}

void CompiledMethod::clear_inline_caches() {
  assert(SafepointSynchronize::is_at_safepoint(), "clearing of IC's only allowed at safepoint");
  RelocIterator iter(this);
  while (iter.next()) {
    iter.reloc()->clear_inline_cache();
  }
}

// Clear IC callsites, releasing ICStubs of all compiled ICs
// as well as any associated CompiledICHolders.
void CompiledMethod::clear_ic_callsites() {
  assert(CompiledICLocker::is_safe(this), "mt unsafe call");
  ResourceMark rm;
  RelocIterator iter(this);
  while(iter.next()) {
    if (iter.type() == relocInfo::virtual_call_type) {
      CompiledIC* ic = CompiledIC_at(&iter);
      ic->set_to_clean(false);
    }
  }
}

#ifdef ASSERT
// Check class_loader is alive for this bit of metadata.
class CheckClass : public MetadataClosure {
  void do_metadata(Metadata* md) {
    Klass* klass = nullptr;
    if (md->is_klass()) {
      klass = ((Klass*)md);
    } else if (md->is_method()) {
      klass = ((Method*)md)->method_holder();
    } else if (md->is_methodData()) {
      klass = ((MethodData*)md)->method()->method_holder();
    } else {
      md->print();
      ShouldNotReachHere();
    }
    assert(klass->is_loader_alive(), "must be alive");
  }
};
#endif // ASSERT


bool CompiledMethod::clean_ic_if_metadata_is_dead(CompiledIC *ic) {
  if (ic->is_clean()) {
    return true;
  }
  if (ic->is_icholder_call()) {
    // The only exception is compiledICHolder metadata which may
    // yet be marked below. (We check this further below).
    CompiledICHolder* cichk_metdata = ic->cached_icholder();

    if (cichk_metdata->is_loader_alive()) {
      return true;
    }
  } else {
    Metadata* ic_metdata = ic->cached_metadata();
    if (ic_metdata != nullptr) {
      if (ic_metdata->is_klass()) {
        if (((Klass*)ic_metdata)->is_loader_alive()) {
          return true;
        }
      } else if (ic_metdata->is_method()) {
        Method* method = (Method*)ic_metdata;
        assert(!method->is_old(), "old method should have been cleaned");
        if (method->method_holder()->is_loader_alive()) {
          return true;
        }
      } else {
        ShouldNotReachHere();
      }
    } else {
      // This inline cache is a megamorphic vtable call. Those ICs never hold
      // any Metadata and should therefore never be cleaned by this function.
      return true;
    }
  }

  return ic->set_to_clean();
}

// Clean references to unloaded nmethods at addr from this one, which is not unloaded.
template <class CompiledICorStaticCall>
static bool clean_if_nmethod_is_unloaded(CompiledICorStaticCall *ic, address addr, CompiledMethod* from,
                                         bool clean_all) {
  CodeBlob *cb = CodeCache::find_blob(addr);
  CompiledMethod* nm = (cb != nullptr) ? cb->as_compiled_method_or_null() : nullptr;
  if (nm != nullptr) {
    // Clean inline caches pointing to bad nmethods
    if (clean_all || !nm->is_in_use() || nm->is_unloading() || (nm->method()->code() != nm)) {
      if (!ic->set_to_clean(!from->is_unloading())) {
        return false;
      }
      assert(ic->is_clean(), "nmethod " PTR_FORMAT "not clean %s", p2i(from), from->method()->name_and_sig_as_C_string());
    }
  }
  return true;
}

static bool clean_if_nmethod_is_unloaded(CompiledIC *ic, CompiledMethod* from,
                                         bool clean_all) {
  return clean_if_nmethod_is_unloaded(ic, ic->ic_destination(), from, clean_all);
}

static bool clean_if_nmethod_is_unloaded(CompiledStaticCall *csc, CompiledMethod* from,
                                         bool clean_all) {
  return clean_if_nmethod_is_unloaded(csc, csc->destination(), from, clean_all);
}

// Cleans caches in nmethods that point to either classes that are unloaded
// or nmethods that are unloaded.
//
// Can be called either in parallel by G1 currently or after all
// nmethods are unloaded.  Return postponed=true in the parallel case for
// inline caches found that point to nmethods that are not yet visited during
// the do_unloading walk.
bool CompiledMethod::unload_nmethod_caches(bool unloading_occurred) {
  ResourceMark rm;

  // Exception cache only needs to be called if unloading occurred
  if (unloading_occurred) {
    clean_exception_cache();
  }

  if (!cleanup_inline_caches_impl(unloading_occurred, false)) {
    return false;
  }

#ifdef ASSERT
  // Check that the metadata embedded in the nmethod is alive
  CheckClass check_class;
  metadata_do(&check_class);
#endif
  return true;
}

void CompiledMethod::run_nmethod_entry_barrier() {
  BarrierSetNMethod* bs_nm = BarrierSet::barrier_set()->barrier_set_nmethod();
  if (bs_nm != nullptr) {
    // We want to keep an invariant that nmethods found through iterations of a Thread's
    // nmethods found in safepoints have gone through an entry barrier and are not armed.
    // By calling this nmethod entry barrier, it plays along and acts
    // like any other nmethod found on the stack of a thread (fewer surprises).
    nmethod* nm = as_nmethod_or_null();
    if (nm != nullptr && bs_nm->is_armed(nm)) {
      bool alive = bs_nm->nmethod_entry_barrier(nm);
      assert(alive, "should be alive");
    }
  }
}

// Only called by whitebox test
void CompiledMethod::cleanup_inline_caches_whitebox() {
  assert_locked_or_safepoint(CodeCache_lock);
  CompiledICLocker ic_locker(this);
  guarantee(cleanup_inline_caches_impl(false /* unloading_occurred */, true /* clean_all */),
            "Inline cache cleaning in a safepoint can't fail");
}

address* CompiledMethod::orig_pc_addr(const frame* fr) {
  return (address*) ((address)fr->unextended_sp() + orig_pc_offset());
}

// Called to clean up after class unloading for live nmethods
bool CompiledMethod::cleanup_inline_caches_impl(bool unloading_occurred, bool clean_all) {
  assert(CompiledICLocker::is_safe(this), "mt unsafe call");
  ResourceMark rm;

  // Find all calls in an nmethod and clear the ones that point to bad nmethods.
  RelocIterator iter(this, oops_reloc_begin());
  bool is_in_static_stub = false;
  while(iter.next()) {

    switch (iter.type()) {

    case relocInfo::virtual_call_type:
      if (unloading_occurred) {
        // If class unloading occurred we first clear ICs where the cached metadata
        // is referring to an unloaded klass or method.
        if (!clean_ic_if_metadata_is_dead(CompiledIC_at(&iter))) {
          return false;
        }
      }

      if (!clean_if_nmethod_is_unloaded(CompiledIC_at(&iter), this, clean_all)) {
        return false;
      }
      break;

    case relocInfo::opt_virtual_call_type:
      if (!clean_if_nmethod_is_unloaded(CompiledIC_at(&iter), this, clean_all)) {
        return false;
      }
      break;

    case relocInfo::static_call_type:
      if (!clean_if_nmethod_is_unloaded(compiledStaticCall_at(iter.reloc()), this, clean_all)) {
        return false;
      }
      break;

    case relocInfo::static_stub_type: {
      is_in_static_stub = true;
      break;
    }

    case relocInfo::metadata_type: {
      // Only the metadata relocations contained in static/opt virtual call stubs
      // contains the Method* passed to c2i adapters. It is the only metadata
      // relocation that needs to be walked, as it is the one metadata relocation
      // that violates the invariant that all metadata relocations have an oop
      // in the compiled method (due to deferred resolution and code patching).

      // This causes dead metadata to remain in compiled methods that are not
      // unloading. Unless these slippery metadata relocations of the static
      // stubs are at least cleared, subsequent class redefinition operations
      // will access potentially free memory, and JavaThread execution
      // concurrent to class unloading may call c2i adapters with dead methods.
      if (!is_in_static_stub) {
        // The first metadata relocation after a static stub relocation is the
        // metadata relocation of the static stub used to pass the Method* to
        // c2i adapters.
        continue;
      }
      is_in_static_stub = false;
      if (is_unloading()) {
        // If the nmethod itself is dying, then it may point at dead metadata.
        // Nobody should follow that metadata; it is strictly unsafe.
        continue;
      }
      metadata_Relocation* r = iter.metadata_reloc();
      Metadata* md = r->metadata_value();
      if (md != nullptr && md->is_method()) {
        Method* method = static_cast<Method*>(md);
        if (!method->method_holder()->is_loader_alive()) {
          Atomic::store(r->metadata_addr(), (Method*)nullptr);

          if (!r->metadata_is_immediate()) {
            r->fix_metadata_relocation();
          }
        }
      }
      break;
    }

    default:
      break;
    }
  }

  return true;
}

address CompiledMethod::continuation_for_implicit_exception(address pc, bool for_div0_check) {
  // Exception happened outside inline-cache check code => we are inside
  // an active nmethod => use cpc to determine a return address
  int exception_offset = pc - code_begin();
  int cont_offset = ImplicitExceptionTable(this).continuation_offset( exception_offset );
#ifdef ASSERT
  if (cont_offset == 0) {
    Thread* thread = Thread::current();
    ResourceMark rm(thread);
    CodeBlob* cb = CodeCache::find_blob(pc);
    assert(cb != nullptr && cb == this, "");
    ttyLocker ttyl;
    tty->print_cr("implicit exception happened at " INTPTR_FORMAT, p2i(pc));
    print();
    method()->print_codes();
    print_code();
    print_pcs();
  }
#endif
  if (cont_offset == 0) {
    // Let the normal error handling report the exception
    return nullptr;
  }
  if (cont_offset == exception_offset) {
#if INCLUDE_JVMCI
    Deoptimization::DeoptReason deopt_reason = for_div0_check ? Deoptimization::Reason_div0_check : Deoptimization::Reason_null_check;
    JavaThread *thread = JavaThread::current();
    thread->set_jvmci_implicit_exception_pc(pc);
    thread->set_pending_deoptimization(Deoptimization::make_trap_request(deopt_reason,
                                                                         Deoptimization::Action_reinterpret));
    return (SharedRuntime::deopt_blob()->implicit_exception_uncommon_trap());
#else
    ShouldNotReachHere();
#endif
  }
  return code_begin() + cont_offset;
}

class HasEvolDependency : public MetadataClosure {
  bool _has_evol_dependency;
 public:
  HasEvolDependency() : _has_evol_dependency(false) {}
  void do_metadata(Metadata* md) {
    if (md->is_method()) {
      Method* method = (Method*)md;
      if (method->is_old()) {
        _has_evol_dependency = true;
      }
    }
  }
  bool has_evol_dependency() const { return _has_evol_dependency; }
};

bool CompiledMethod::has_evol_metadata() {
  // Check the metadata in relocIter and CompiledIC and also deoptimize
  // any nmethod that has reference to old methods.
  HasEvolDependency check_evol;
  metadata_do(&check_evol);
  if (check_evol.has_evol_dependency() && log_is_enabled(Debug, redefine, class, nmethod)) {
    ResourceMark rm;
    log_debug(redefine, class, nmethod)
            ("Found evol dependency of nmethod %s.%s(%s) compile_id=%d on in nmethod metadata",
             _method->method_holder()->external_name(),
             _method->name()->as_C_string(),
             _method->signature()->as_C_string(),
             compile_id());
  }
  return check_evol.has_evol_dependency();
}
