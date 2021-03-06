/*
 * Copyright (c) 1997, 2016, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoader.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "gc/shared/vmGCOperations.hpp"
#include "interpreter/interpreter.hpp"
#include "logging/log.hpp"
#include "logging/logStream.inline.hpp"
#include "memory/allocation.inline.hpp"
#ifdef ASSERT
#include "memory/guardedMemory.hpp"
#endif
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvm.h"
#include "prims/jvm_misc.hpp"
#include "prims/privilegedStack.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vm_version.hpp"
#include "services/attachListener.hpp"
#include "services/mallocTracker.hpp"
#include "services/memTracker.hpp"
#include "services/nmtCommon.hpp"
#include "services/threadService.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"

# include <signal.h>
# include <errno.h>

OSThread*         os::_starting_thread    = NULL;
address           os::_polling_page       = NULL;
volatile int32_t* os::_mem_serialize_page = NULL;
uintptr_t         os::_serialize_page_mask = 0;
long              os::_rand_seed          = 1;
int               os::_processor_count    = 0;
int               os::_initial_active_processor_count = 0;
size_t            os::_page_sizes[os::page_sizes_max];

#ifndef PRODUCT
julong os::num_mallocs = 0;         // # of calls to malloc/realloc
julong os::alloc_bytes = 0;         // # of bytes allocated
julong os::num_frees = 0;           // # of calls to free
julong os::free_bytes = 0;          // # of bytes freed
#endif

static juint cur_malloc_words = 0;  // current size for MallocMaxTestWords

void os_init_globals() {
  // Called from init_globals().
  // See Threads::create_vm() in thread.cpp, and init.cpp.
  os::init_globals();
}

// Fill in buffer with current local time as an ISO-8601 string.
// E.g., yyyy-mm-ddThh:mm:ss-zzzz.
// Returns buffer, or NULL if it failed.
// This would mostly be a call to
//     strftime(...., "%Y-%m-%d" "T" "%H:%M:%S" "%z", ....)
// except that on Windows the %z behaves badly, so we do it ourselves.
// Also, people wanted milliseconds on there,
// and strftime doesn't do milliseconds.
char* os::iso8601_time(char* buffer, size_t buffer_length) {
  // Output will be of the form "YYYY-MM-DDThh:mm:ss.mmm+zzzz\0"
  //                                      1         2
  //                             12345678901234567890123456789
  // format string: "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d%02d"
  static const size_t needed_buffer = 29;

  // Sanity check the arguments
  if (buffer == NULL) {
    assert(false, "NULL buffer");
    return NULL;
  }
  if (buffer_length < needed_buffer) {
    assert(false, "buffer_length too small");
    return NULL;
  }
  // Get the current time
  jlong milliseconds_since_19700101 = javaTimeMillis();
  const int milliseconds_per_microsecond = 1000;
  const time_t seconds_since_19700101 =
    milliseconds_since_19700101 / milliseconds_per_microsecond;
  const int milliseconds_after_second =
    milliseconds_since_19700101 % milliseconds_per_microsecond;
  // Convert the time value to a tm and timezone variable
  struct tm time_struct;
  if (localtime_pd(&seconds_since_19700101, &time_struct) == NULL) {
    assert(false, "Failed localtime_pd");
    return NULL;
  }
#if defined(_ALLBSD_SOURCE)
  const time_t zone = (time_t) time_struct.tm_gmtoff;
#else
  const time_t zone = timezone;
#endif

  // If daylight savings time is in effect,
  // we are 1 hour East of our time zone
  const time_t seconds_per_minute = 60;
  const time_t minutes_per_hour = 60;
  const time_t seconds_per_hour = seconds_per_minute * minutes_per_hour;
  time_t UTC_to_local = zone;
  if (time_struct.tm_isdst > 0) {
    UTC_to_local = UTC_to_local - seconds_per_hour;
  }
  // Compute the time zone offset.
  //    localtime_pd() sets timezone to the difference (in seconds)
  //    between UTC and and local time.
  //    ISO 8601 says we need the difference between local time and UTC,
  //    we change the sign of the localtime_pd() result.
  const time_t local_to_UTC = -(UTC_to_local);
  // Then we have to figure out if if we are ahead (+) or behind (-) UTC.
  char sign_local_to_UTC = '+';
  time_t abs_local_to_UTC = local_to_UTC;
  if (local_to_UTC < 0) {
    sign_local_to_UTC = '-';
    abs_local_to_UTC = -(abs_local_to_UTC);
  }
  // Convert time zone offset seconds to hours and minutes.
  const time_t zone_hours = (abs_local_to_UTC / seconds_per_hour);
  const time_t zone_min =
    ((abs_local_to_UTC % seconds_per_hour) / seconds_per_minute);

  // Print an ISO 8601 date and time stamp into the buffer
  const int year = 1900 + time_struct.tm_year;
  const int month = 1 + time_struct.tm_mon;
  const int printed = jio_snprintf(buffer, buffer_length,
                                   "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d%02d",
                                   year,
                                   month,
                                   time_struct.tm_mday,
                                   time_struct.tm_hour,
                                   time_struct.tm_min,
                                   time_struct.tm_sec,
                                   milliseconds_after_second,
                                   sign_local_to_UTC,
                                   zone_hours,
                                   zone_min);
  if (printed == 0) {
    assert(false, "Failed jio_printf");
    return NULL;
  }
  return buffer;
}

OSReturn os::set_priority(Thread* thread, ThreadPriority p) {
#ifdef ASSERT
  if (!(!thread->is_Java_thread() ||
         Thread::current() == thread  ||
         Threads_lock->owned_by_self()
         || thread->is_Compiler_thread()
        )) {
    assert(false, "possibility of dangling Thread pointer");
  }
#endif

  if (p >= MinPriority && p <= MaxPriority) {
    int priority = java_to_os_priority[p];
    return set_native_priority(thread, priority);
  } else {
    assert(false, "Should not happen");
    return OS_ERR;
  }
}

// The mapping from OS priority back to Java priority may be inexact because
// Java priorities can map M:1 with native priorities. If you want the definite
// Java priority then use JavaThread::java_priority()
OSReturn os::get_priority(const Thread* const thread, ThreadPriority& priority) {
  int p;
  int os_prio;
  OSReturn ret = get_native_priority(thread, &os_prio);
  if (ret != OS_OK) return ret;

  if (java_to_os_priority[MaxPriority] > java_to_os_priority[MinPriority]) {
    for (p = MaxPriority; p > MinPriority && java_to_os_priority[p] > os_prio; p--) ;
  } else {
    // niceness values are in reverse order
    for (p = MaxPriority; p > MinPriority && java_to_os_priority[p] < os_prio; p--) ;
  }
  priority = (ThreadPriority)p;
  return OS_OK;
}


// --------------------- sun.misc.Signal (optional) ---------------------


// SIGBREAK is sent by the keyboard to query the VM state
#ifndef SIGBREAK
#define SIGBREAK SIGQUIT
#endif

// sigexitnum_pd is a platform-specific special signal used for terminating the Signal thread.


static void signal_thread_entry(JavaThread* thread, TRAPS) {
  os::set_priority(thread, NearMaxPriority);
  while (true) {
    int sig;
    {
      // FIXME : Currently we have not decided what should be the status
      //         for this java thread blocked here. Once we decide about
      //         that we should fix this.
      sig = os::signal_wait();
    }
    if (sig == os::sigexitnum_pd()) {
       // Terminate the signal thread
       return;
    }

    switch (sig) {
      case SIGBREAK: {
        // Check if the signal is a trigger to start the Attach Listener - in that
        // case don't print stack traces.
        if (!DisableAttachMechanism && AttachListener::is_init_trigger()) {
          continue;
        }
        // Print stack traces
        // Any SIGBREAK operations added here should make sure to flush
        // the output stream (e.g. tty->flush()) after output.  See 4803766.
        // Each module also prints an extra carriage return after its output.
        VM_PrintThreads op;
        VMThread::execute(&op);
        VM_PrintJNI jni_op;
        VMThread::execute(&jni_op);
        VM_FindDeadlocks op1(tty);
        VMThread::execute(&op1);
        Universe::print_heap_at_SIGBREAK();
        if (PrintClassHistogram) {
          VM_GC_HeapInspection op1(tty, true /* force full GC before heap inspection */);
          VMThread::execute(&op1);
        }
        if (JvmtiExport::should_post_data_dump()) {
          JvmtiExport::post_data_dump();
        }
        break;
      }
      default: {
        // Dispatch the signal to java
        HandleMark hm(THREAD);
        Klass* k = SystemDictionary::resolve_or_null(vmSymbols::jdk_internal_misc_Signal(), THREAD);
        KlassHandle klass (THREAD, k);
        if (klass.not_null()) {
          JavaValue result(T_VOID);
          JavaCallArguments args;
          args.push_int(sig);
          JavaCalls::call_static(
            &result,
            klass,
            vmSymbols::dispatch_name(),
            vmSymbols::int_void_signature(),
            &args,
            THREAD
          );
        }
        if (HAS_PENDING_EXCEPTION) {
          // tty is initialized early so we don't expect it to be null, but
          // if it is we can't risk doing an initialization that might
          // trigger additional out-of-memory conditions
          if (tty != NULL) {
            char klass_name[256];
            char tmp_sig_name[16];
            const char* sig_name = "UNKNOWN";
            InstanceKlass::cast(PENDING_EXCEPTION->klass())->
              name()->as_klass_external_name(klass_name, 256);
            if (os::exception_name(sig, tmp_sig_name, 16) != NULL)
              sig_name = tmp_sig_name;
            warning("Exception %s occurred dispatching signal %s to handler"
                    "- the VM may need to be forcibly terminated",
                    klass_name, sig_name );
          }
          CLEAR_PENDING_EXCEPTION;
        }
      }
    }
  }
}

void os::init_before_ergo() {
  initialize_initial_active_processor_count();
  // We need to initialize large page support here because ergonomics takes some
  // decisions depending on large page support and the calculated large page size.
  large_page_init();

  // We need to adapt the configured number of stack protection pages given
  // in 4K pages to the actual os page size. We must do this before setting
  // up minimal stack sizes etc. in os::init_2().
  JavaThread::set_stack_red_zone_size     (align_size_up(StackRedPages      * 4 * K, vm_page_size()));
  JavaThread::set_stack_yellow_zone_size  (align_size_up(StackYellowPages   * 4 * K, vm_page_size()));
  JavaThread::set_stack_reserved_zone_size(align_size_up(StackReservedPages * 4 * K, vm_page_size()));
  JavaThread::set_stack_shadow_zone_size  (align_size_up(StackShadowPages   * 4 * K, vm_page_size()));

  // VM version initialization identifies some characteristics of the
  // platform that are used during ergonomic decisions.
  VM_Version::init_before_ergo();
}

void os::signal_init() {
  if (!ReduceSignalUsage) {
    // Setup JavaThread for processing signals
    EXCEPTION_MARK;
    Klass* k = SystemDictionary::resolve_or_fail(vmSymbols::java_lang_Thread(), true, CHECK);
    instanceKlassHandle klass (THREAD, k);
    instanceHandle thread_oop = klass->allocate_instance_handle(CHECK);

    const char thread_name[] = "Signal Dispatcher";
    Handle string = java_lang_String::create_from_str(thread_name, CHECK);

    // Initialize thread_oop to put it into the system threadGroup
    Handle thread_group (THREAD, Universe::system_thread_group());
    JavaValue result(T_VOID);
    JavaCalls::call_special(&result, thread_oop,
                           klass,
                           vmSymbols::object_initializer_name(),
                           vmSymbols::threadgroup_string_void_signature(),
                           thread_group,
                           string,
                           CHECK);

    KlassHandle group(THREAD, SystemDictionary::ThreadGroup_klass());
    JavaCalls::call_special(&result,
                            thread_group,
                            group,
                            vmSymbols::add_method_name(),
                            vmSymbols::thread_void_signature(),
                            thread_oop,         // ARG 1
                            CHECK);

    os::signal_init_pd();

    { MutexLocker mu(Threads_lock);
      JavaThread* signal_thread = new JavaThread(&signal_thread_entry);

      // At this point it may be possible that no osthread was created for the
      // JavaThread due to lack of memory. We would have to throw an exception
      // in that case. However, since this must work and we do not allow
      // exceptions anyway, check and abort if this fails.
      if (signal_thread == NULL || signal_thread->osthread() == NULL) {
        vm_exit_during_initialization("java.lang.OutOfMemoryError",
                                      os::native_thread_creation_failed_msg());
      }

      java_lang_Thread::set_thread(thread_oop(), signal_thread);
      java_lang_Thread::set_priority(thread_oop(), NearMaxPriority);
      java_lang_Thread::set_daemon(thread_oop());

      signal_thread->set_threadObj(thread_oop());
      Threads::add(signal_thread);
      Thread::start(signal_thread);
    }
    // Handle ^BREAK
    os::signal(SIGBREAK, os::user_handler());
  }
}


void os::terminate_signal_thread() {
  if (!ReduceSignalUsage)
    signal_notify(sigexitnum_pd());
}


// --------------------- loading libraries ---------------------

typedef jint (JNICALL *JNI_OnLoad_t)(JavaVM *, void *);
extern struct JavaVM_ main_vm;

static void* _native_java_library = NULL;

void* os::native_java_library() {
  if (_native_java_library == NULL) {
    char buffer[JVM_MAXPATHLEN];
    char ebuf[1024];

    // Try to load verify dll first. In 1.3 java dll depends on it and is not
    // always able to find it when the loading executable is outside the JDK.
    // In order to keep working with 1.2 we ignore any loading errors.
    if (dll_build_name(buffer, sizeof(buffer), Arguments::get_dll_dir(),
                       "verify")) {
      dll_load(buffer, ebuf, sizeof(ebuf));
    }

    // Load java dll
    if (dll_build_name(buffer, sizeof(buffer), Arguments::get_dll_dir(),
                       "java")) {
      _native_java_library = dll_load(buffer, ebuf, sizeof(ebuf));
    }
    if (_native_java_library == NULL) {
      vm_exit_during_initialization("Unable to load native library", ebuf);
    }

#if defined(__OpenBSD__)
    // Work-around OpenBSD's lack of $ORIGIN support by pre-loading libnet.so
    // ignore errors
    if (dll_build_name(buffer, sizeof(buffer), Arguments::get_dll_dir(),
                       "net")) {
      dll_load(buffer, ebuf, sizeof(ebuf));
    }
#endif
  }
  return _native_java_library;
}

/*
 * Support for finding Agent_On(Un)Load/Attach<_lib_name> if it exists.
 * If check_lib == true then we are looking for an
 * Agent_OnLoad_lib_name or Agent_OnAttach_lib_name function to determine if
 * this library is statically linked into the image.
 * If check_lib == false then we will look for the appropriate symbol in the
 * executable if agent_lib->is_static_lib() == true or in the shared library
 * referenced by 'handle'.
 */
void* os::find_agent_function(AgentLibrary *agent_lib, bool check_lib,
                              const char *syms[], size_t syms_len) {
  assert(agent_lib != NULL, "sanity check");
  const char *lib_name;
  void *handle = agent_lib->os_lib();
  void *entryName = NULL;
  char *agent_function_name;
  size_t i;

  // If checking then use the agent name otherwise test is_static_lib() to
  // see how to process this lookup
  lib_name = ((check_lib || agent_lib->is_static_lib()) ? agent_lib->name() : NULL);
  for (i = 0; i < syms_len; i++) {
    agent_function_name = build_agent_function_name(syms[i], lib_name, agent_lib->is_absolute_path());
    if (agent_function_name == NULL) {
      break;
    }
    entryName = dll_lookup(handle, agent_function_name);
    FREE_C_HEAP_ARRAY(char, agent_function_name);
    if (entryName != NULL) {
      break;
    }
  }
  return entryName;
}

// See if the passed in agent is statically linked into the VM image.
bool os::find_builtin_agent(AgentLibrary *agent_lib, const char *syms[],
                            size_t syms_len) {
  void *ret;
  void *proc_handle;
  void *save_handle;

  assert(agent_lib != NULL, "sanity check");
  if (agent_lib->name() == NULL) {
    return false;
  }
  proc_handle = get_default_process_handle();
  // Check for Agent_OnLoad/Attach_lib_name function
  save_handle = agent_lib->os_lib();
  // We want to look in this process' symbol table.
  agent_lib->set_os_lib(proc_handle);
  ret = find_agent_function(agent_lib, true, syms, syms_len);
  if (ret != NULL) {
    // Found an entry point like Agent_OnLoad_lib_name so we have a static agent
    agent_lib->set_valid();
    agent_lib->set_static_lib(true);
    return true;
  }
  agent_lib->set_os_lib(save_handle);
  return false;
}

// --------------------- heap allocation utilities ---------------------

char *os::strdup(const char *str, MEMFLAGS flags) {
  size_t size = strlen(str);
  char *dup_str = (char *)malloc(size + 1, flags);
  if (dup_str == NULL) return NULL;
  strcpy(dup_str, str);
  return dup_str;
}

char* os::strdup_check_oom(const char* str, MEMFLAGS flags) {
  char* p = os::strdup(str, flags);
  if (p == NULL) {
    vm_exit_out_of_memory(strlen(str) + 1, OOM_MALLOC_ERROR, "os::strdup_check_oom");
  }
  return p;
}


#define paranoid                 0  /* only set to 1 if you suspect checking code has bug */

#ifdef ASSERT

static void verify_memory(void* ptr) {
  GuardedMemory guarded(ptr);
  if (!guarded.verify_guards()) {
    tty->print_cr("## nof_mallocs = " UINT64_FORMAT ", nof_frees = " UINT64_FORMAT, os::num_mallocs, os::num_frees);
    tty->print_cr("## memory stomp:");
    guarded.print_on(tty);
    fatal("memory stomping error");
  }
}

#endif

//
// This function supports testing of the malloc out of memory
// condition without really running the system out of memory.
//
static bool has_reached_max_malloc_test_peak(size_t alloc_size) {
  if (MallocMaxTestWords > 0) {
    jint words = (jint)(alloc_size / BytesPerWord);

    if ((cur_malloc_words + words) > MallocMaxTestWords) {
      return true;
    }
    Atomic::add(words, (volatile jint *)&cur_malloc_words);
  }
  return false;
}

void* os::malloc(size_t size, MEMFLAGS flags) {
  return os::malloc(size, flags, CALLER_PC);
}

void* os::malloc(size_t size, MEMFLAGS memflags, const NativeCallStack& stack) {
  NOT_PRODUCT(inc_stat_counter(&num_mallocs, 1));
  NOT_PRODUCT(inc_stat_counter(&alloc_bytes, size));

#ifdef ASSERT
  // checking for the WatcherThread and crash_protection first
  // since os::malloc can be called when the libjvm.{dll,so} is
  // first loaded and we don't have a thread yet.
  // try to find the thread after we see that the watcher thread
  // exists and has crash protection.
  WatcherThread *wt = WatcherThread::watcher_thread();
  if (wt != NULL && wt->has_crash_protection()) {
    Thread* thread = Thread::current_or_null();
    if (thread == wt) {
      assert(!wt->has_crash_protection(),
          "Can't malloc with crash protection from WatcherThread");
    }
  }
#endif

  if (size == 0) {
    // return a valid pointer if size is zero
    // if NULL is returned the calling functions assume out of memory.
    size = 1;
  }

  // NMT support
  NMT_TrackingLevel level = MemTracker::tracking_level();
  size_t            nmt_header_size = MemTracker::malloc_header_size(level);

#ifndef ASSERT
  const size_t alloc_size = size + nmt_header_size;
#else
  const size_t alloc_size = GuardedMemory::get_total_size(size + nmt_header_size);
  if (size + nmt_header_size > alloc_size) { // Check for rollover.
    return NULL;
  }
#endif

  // For the test flag -XX:MallocMaxTestWords
  if (has_reached_max_malloc_test_peak(size)) {
    return NULL;
  }

  u_char* ptr;
  ptr = (u_char*)::malloc(alloc_size);

#ifdef ASSERT
  if (ptr == NULL) {
    return NULL;
  }
  // Wrap memory with guard
  GuardedMemory guarded(ptr, size + nmt_header_size);
  ptr = guarded.get_user_ptr();
#endif
  if ((intptr_t)ptr == (intptr_t)MallocCatchPtr) {
    tty->print_cr("os::malloc caught, " SIZE_FORMAT " bytes --> " PTR_FORMAT, size, p2i(ptr));
    breakpoint();
  }
  debug_only(if (paranoid) verify_memory(ptr));
  if (PrintMalloc && tty != NULL) {
    tty->print_cr("os::malloc " SIZE_FORMAT " bytes --> " PTR_FORMAT, size, p2i(ptr));
  }

  // we do not track guard memory
  return MemTracker::record_malloc((address)ptr, size, memflags, stack, level);
}

void* os::realloc(void *memblock, size_t size, MEMFLAGS flags) {
  return os::realloc(memblock, size, flags, CALLER_PC);
}

void* os::realloc(void *memblock, size_t size, MEMFLAGS memflags, const NativeCallStack& stack) {

  // For the test flag -XX:MallocMaxTestWords
  if (has_reached_max_malloc_test_peak(size)) {
    return NULL;
  }

#ifndef ASSERT
  NOT_PRODUCT(inc_stat_counter(&num_mallocs, 1));
  NOT_PRODUCT(inc_stat_counter(&alloc_bytes, size));
   // NMT support
  void* membase = MemTracker::record_free(memblock);
  NMT_TrackingLevel level = MemTracker::tracking_level();
  size_t  nmt_header_size = MemTracker::malloc_header_size(level);
  void* ptr = ::realloc(membase, size + nmt_header_size);
  return MemTracker::record_malloc(ptr, size, memflags, stack, level);
#else
  if (memblock == NULL) {
    return os::malloc(size, memflags, stack);
  }
  if ((intptr_t)memblock == (intptr_t)MallocCatchPtr) {
    tty->print_cr("os::realloc caught " PTR_FORMAT, p2i(memblock));
    breakpoint();
  }
  // NMT support
  void* membase = MemTracker::malloc_base(memblock);
  verify_memory(membase);
  if (size == 0) {
    return NULL;
  }
  // always move the block
  void* ptr = os::malloc(size, memflags, stack);
  if (PrintMalloc && tty != NULL) {
    tty->print_cr("os::realloc " SIZE_FORMAT " bytes, " PTR_FORMAT " --> " PTR_FORMAT, size, p2i(memblock), p2i(ptr));
  }
  // Copy to new memory if malloc didn't fail
  if ( ptr != NULL ) {
    GuardedMemory guarded(MemTracker::malloc_base(memblock));
    // Guard's user data contains NMT header
    size_t memblock_size = guarded.get_user_size() - MemTracker::malloc_header_size(memblock);
    memcpy(ptr, memblock, MIN2(size, memblock_size));
    if (paranoid) verify_memory(MemTracker::malloc_base(ptr));
    if ((intptr_t)ptr == (intptr_t)MallocCatchPtr) {
      tty->print_cr("os::realloc caught, " SIZE_FORMAT " bytes --> " PTR_FORMAT, size, p2i(ptr));
      breakpoint();
    }
    os::free(memblock);
  }
  return ptr;
#endif
}


void  os::free(void *memblock) {
  NOT_PRODUCT(inc_stat_counter(&num_frees, 1));
#ifdef ASSERT
  if (memblock == NULL) return;
  if ((intptr_t)memblock == (intptr_t)MallocCatchPtr) {
    if (tty != NULL) tty->print_cr("os::free caught " PTR_FORMAT, p2i(memblock));
    breakpoint();
  }
  void* membase = MemTracker::record_free(memblock);
  verify_memory(membase);

  GuardedMemory guarded(membase);
  size_t size = guarded.get_user_size();
  inc_stat_counter(&free_bytes, size);
  membase = guarded.release_for_freeing();
  if (PrintMalloc && tty != NULL) {
      fprintf(stderr, "os::free " SIZE_FORMAT " bytes --> " PTR_FORMAT "\n", size, (uintptr_t)membase);
  }
  ::free(membase);
#else
  void* membase = MemTracker::record_free(memblock);
  ::free(membase);
#endif
}

void os::init_random(long initval) {
  _rand_seed = initval;
}


long os::random() {
  /* standard, well-known linear congruential random generator with
   * next_rand = (16807*seed) mod (2**31-1)
   * see
   * (1) "Random Number Generators: Good Ones Are Hard to Find",
   *      S.K. Park and K.W. Miller, Communications of the ACM 31:10 (Oct 1988),
   * (2) "Two Fast Implementations of the 'Minimal Standard' Random
   *     Number Generator", David G. Carta, Comm. ACM 33, 1 (Jan 1990), pp. 87-88.
  */
  const long a = 16807;
  const unsigned long m = 2147483647;
  const long q = m / a;        assert(q == 127773, "weird math");
  const long r = m % a;        assert(r == 2836, "weird math");

  // compute az=2^31p+q
  unsigned long lo = a * (long)(_rand_seed & 0xFFFF);
  unsigned long hi = a * (long)((unsigned long)_rand_seed >> 16);
  lo += (hi & 0x7FFF) << 16;

  // if q overflowed, ignore the overflow and increment q
  if (lo > m) {
    lo &= m;
    ++lo;
  }
  lo += hi >> 15;

  // if (p+q) overflowed, ignore the overflow and increment (p+q)
  if (lo > m) {
    lo &= m;
    ++lo;
  }
  return (_rand_seed = lo);
}

// The INITIALIZED state is distinguished from the SUSPENDED state because the
// conditions in which a thread is first started are different from those in which
// a suspension is resumed.  These differences make it hard for us to apply the
// tougher checks when starting threads that we want to do when resuming them.
// However, when start_thread is called as a result of Thread.start, on a Java
// thread, the operation is synchronized on the Java Thread object.  So there
// cannot be a race to start the thread and hence for the thread to exit while
// we are working on it.  Non-Java threads that start Java threads either have
// to do so in a context in which races are impossible, or should do appropriate
// locking.

void os::start_thread(Thread* thread) {
  // guard suspend/resume
  MutexLockerEx ml(thread->SR_lock(), Mutex::_no_safepoint_check_flag);
  OSThread* osthread = thread->osthread();
  osthread->set_state(RUNNABLE);
  pd_start_thread(thread);
}

void os::abort(bool dump_core) {
  abort(dump_core && CreateCoredumpOnCrash, NULL, NULL);
}

//---------------------------------------------------------------------------
// Helper functions for fatal error handler

void os::print_hex_dump(outputStream* st, address start, address end, int unitsize) {
  assert(unitsize == 1 || unitsize == 2 || unitsize == 4 || unitsize == 8, "just checking");

  int cols = 0;
  int cols_per_line = 0;
  switch (unitsize) {
    case 1: cols_per_line = 16; break;
    case 2: cols_per_line = 8;  break;
    case 4: cols_per_line = 4;  break;
    case 8: cols_per_line = 2;  break;
    default: return;
  }

  address p = start;
  st->print(PTR_FORMAT ":   ", p2i(start));
  while (p < end) {
    switch (unitsize) {
      case 1: st->print("%02x", *(u1*)p); break;
      case 2: st->print("%04x", *(u2*)p); break;
      case 4: st->print("%08x", *(u4*)p); break;
      case 8: st->print("%016" FORMAT64_MODIFIER "x", *(u8*)p); break;
    }
    p += unitsize;
    cols++;
    if (cols >= cols_per_line && p < end) {
       cols = 0;
       st->cr();
       st->print(PTR_FORMAT ":   ", p2i(p));
    } else {
       st->print(" ");
    }
  }
  st->cr();
}

void os::print_environment_variables(outputStream* st, const char** env_list) {
  if (env_list) {
    st->print_cr("Environment Variables:");

    for (int i = 0; env_list[i] != NULL; i++) {
      char *envvar = ::getenv(env_list[i]);
      if (envvar != NULL) {
        st->print("%s", env_list[i]);
        st->print("=");
        st->print_cr("%s", envvar);
      }
    }
  }
}

void os::print_cpu_info(outputStream* st, char* buf, size_t buflen) {
  // cpu
  st->print("CPU:");
  st->print("total %d", os::processor_count());
  // It's not safe to query number of active processors after crash
  // st->print("(active %d)", os::active_processor_count()); but we can
  // print the initial number of active processors.
  // We access the raw value here because the assert in the accessor will
  // fail if the crash occurs before initialization of this value.
  st->print(" (initial active %d)", _initial_active_processor_count);
  st->print(" %s", VM_Version::features_string());
  st->cr();
  pd_print_cpu_info(st, buf, buflen);
}

// Print a one line string summarizing the cpu, number of cores, memory, and operating system version
void os::print_summary_info(outputStream* st, char* buf, size_t buflen) {
  st->print("Host: ");
#ifndef PRODUCT
  if (get_host_name(buf, buflen)) {
    st->print("%s, ", buf);
  }
#endif // PRODUCT
  get_summary_cpu_info(buf, buflen);
  st->print("%s, ", buf);
  size_t mem = physical_memory()/G;
  if (mem == 0) {  // for low memory systems
    mem = physical_memory()/M;
    st->print("%d cores, " SIZE_FORMAT "M, ", processor_count(), mem);
  } else {
    st->print("%d cores, " SIZE_FORMAT "G, ", processor_count(), mem);
  }
  get_summary_os_info(buf, buflen);
  st->print_raw(buf);
  st->cr();
}

void os::print_date_and_time(outputStream *st, char* buf, size_t buflen) {
  const int secs_per_day  = 86400;
  const int secs_per_hour = 3600;
  const int secs_per_min  = 60;

  time_t tloc;
  (void)time(&tloc);
  char* timestring = ctime(&tloc);  // ctime adds newline.
  // edit out the newline
  char* nl = strchr(timestring, '\n');
  if (nl != NULL) {
    *nl = '\0';
  }

  struct tm tz;
  if (localtime_pd(&tloc, &tz) != NULL) {
    ::strftime(buf, buflen, "%Z", &tz);
    st->print("Time: %s %s", timestring, buf);
  } else {
    st->print("Time: %s", timestring);
  }

  double t = os::elapsedTime();
  // NOTE: It tends to crash after a SEGV if we want to printf("%f",...) in
  //       Linux. Must be a bug in glibc ? Workaround is to round "t" to int
  //       before printf. We lost some precision, but who cares?
  int eltime = (int)t;  // elapsed time in seconds

  // print elapsed time in a human-readable format:
  int eldays = eltime / secs_per_day;
  int day_secs = eldays * secs_per_day;
  int elhours = (eltime - day_secs) / secs_per_hour;
  int hour_secs = elhours * secs_per_hour;
  int elmins = (eltime - day_secs - hour_secs) / secs_per_min;
  int minute_secs = elmins * secs_per_min;
  int elsecs = (eltime - day_secs - hour_secs - minute_secs);
  st->print_cr(" elapsed time: %d seconds (%dd %dh %dm %ds)", eltime, eldays, elhours, elmins, elsecs);
}

// moved from debug.cpp (used to be find()) but still called from there
// The verbose parameter is only set by the debug code in one case
void os::print_location(outputStream* st, intptr_t x, bool verbose) {
  address addr = (address)x;
  CodeBlob* b = CodeCache::find_blob_unsafe(addr);
  if (b != NULL) {
    if (b->is_buffer_blob()) {
      // the interpreter is generated into a buffer blob
      InterpreterCodelet* i = Interpreter::codelet_containing(addr);
      if (i != NULL) {
        st->print_cr(INTPTR_FORMAT " is at code_begin+%d in an Interpreter codelet", p2i(addr), (int)(addr - i->code_begin()));
        i->print_on(st);
        return;
      }
      if (Interpreter::contains(addr)) {
        st->print_cr(INTPTR_FORMAT " is pointing into interpreter code"
                     " (not bytecode specific)", p2i(addr));
        return;
      }
      //
      if (AdapterHandlerLibrary::contains(b)) {
        st->print_cr(INTPTR_FORMAT " is at code_begin+%d in an AdapterHandler", p2i(addr), (int)(addr - b->code_begin()));
        AdapterHandlerLibrary::print_handler_on(st, b);
      }
      // the stubroutines are generated into a buffer blob
      StubCodeDesc* d = StubCodeDesc::desc_for(addr);
      if (d != NULL) {
        st->print_cr(INTPTR_FORMAT " is at begin+%d in a stub", p2i(addr), (int)(addr - d->begin()));
        d->print_on(st);
        st->cr();
        return;
      }
      if (StubRoutines::contains(addr)) {
        st->print_cr(INTPTR_FORMAT " is pointing to an (unnamed) stub routine", p2i(addr));
        return;
      }
      // the InlineCacheBuffer is using stubs generated into a buffer blob
      if (InlineCacheBuffer::contains(addr)) {
        st->print_cr(INTPTR_FORMAT " is pointing into InlineCacheBuffer", p2i(addr));
        return;
      }
      VtableStub* v = VtableStubs::stub_containing(addr);
      if (v != NULL) {
        st->print_cr(INTPTR_FORMAT " is at entry_point+%d in a vtable stub", p2i(addr), (int)(addr - v->entry_point()));
        v->print_on(st);
        st->cr();
        return;
      }
    }
    nmethod* nm = b->as_nmethod_or_null();
    if (nm != NULL) {
      ResourceMark rm;
      st->print(INTPTR_FORMAT " is at entry_point+%d in (nmethod*)" INTPTR_FORMAT,
                p2i(addr), (int)(addr - nm->entry_point()), p2i(nm));
      if (verbose) {
        st->print(" for ");
        nm->method()->print_value_on(st);
      }
      st->cr();
      nm->print_nmethod(verbose);
      return;
    }
    st->print_cr(INTPTR_FORMAT " is at code_begin+%d in ", p2i(addr), (int)(addr - b->code_begin()));
    b->print_on(st);
    return;
  }

  if (Universe::heap()->is_in(addr)) {
    HeapWord* p = Universe::heap()->block_start(addr);
    bool print = false;
    // If we couldn't find it it just may mean that heap wasn't parsable
    // See if we were just given an oop directly
    if (p != NULL && Universe::heap()->block_is_obj(p)) {
      print = true;
    } else if (p == NULL && ((oopDesc*)addr)->is_oop()) {
      p = (HeapWord*) addr;
      print = true;
    }
    if (print) {
      if (p == (HeapWord*) addr) {
        st->print_cr(INTPTR_FORMAT " is an oop", p2i(addr));
      } else {
        st->print_cr(INTPTR_FORMAT " is pointing into object: " INTPTR_FORMAT, p2i(addr), p2i(p));
      }
      oop(p)->print_on(st);
      return;
    }
  } else {
    if (Universe::heap()->is_in_reserved(addr)) {
      st->print_cr(INTPTR_FORMAT " is an unallocated location "
                   "in the heap", p2i(addr));
      return;
    }
  }
  if (JNIHandles::is_global_handle((jobject) addr)) {
    st->print_cr(INTPTR_FORMAT " is a global jni handle", p2i(addr));
    return;
  }
  if (JNIHandles::is_weak_global_handle((jobject) addr)) {
    st->print_cr(INTPTR_FORMAT " is a weak global jni handle", p2i(addr));
    return;
  }
#ifndef PRODUCT
  // we don't keep the block list in product mode
  if (JNIHandleBlock::any_contains((jobject) addr)) {
    st->print_cr(INTPTR_FORMAT " is a local jni handle", p2i(addr));
    return;
  }
#endif

  for(JavaThread *thread = Threads::first(); thread; thread = thread->next()) {
    // Check for privilege stack
    if (thread->privileged_stack_top() != NULL &&
        thread->privileged_stack_top()->contains(addr)) {
      st->print_cr(INTPTR_FORMAT " is pointing into the privilege stack "
                   "for thread: " INTPTR_FORMAT, p2i(addr), p2i(thread));
      if (verbose) thread->print_on(st);
      return;
    }
    // If the addr is a java thread print information about that.
    if (addr == (address)thread) {
      if (verbose) {
        thread->print_on(st);
      } else {
        st->print_cr(INTPTR_FORMAT " is a thread", p2i(addr));
      }
      return;
    }
    // If the addr is in the stack region for this thread then report that
    // and print thread info
    if (thread->on_local_stack(addr)) {
      st->print_cr(INTPTR_FORMAT " is pointing into the stack for thread: "
                   INTPTR_FORMAT, p2i(addr), p2i(thread));
      if (verbose) thread->print_on(st);
      return;
    }

  }

  // Check if in metaspace and print types that have vptrs (only method now)
  if (Metaspace::contains(addr)) {
    if (Method::has_method_vptr((const void*)addr)) {
      ((Method*)addr)->print_value_on(st);
      st->cr();
    } else {
      // Use addr->print() from the debugger instead (not here)
      st->print_cr(INTPTR_FORMAT " is pointing into metadata", p2i(addr));
    }
    return;
  }

  // Try an OS specific find
  if (os::find(addr, st)) {
    return;
  }

  st->print_cr(INTPTR_FORMAT " is an unknown value", p2i(addr));
}

// Looks like all platforms except IA64 can use the same function to check
// if C stack is walkable beyond current frame. The check for fp() is not
// necessary on Sparc, but it's harmless.
bool os::is_first_C_frame(frame* fr) {
#if (defined(IA64) && !defined(AIX)) && !defined(_WIN32)
  // On IA64 we have to check if the callers bsp is still valid
  // (i.e. within the register stack bounds).
  // Notice: this only works for threads created by the VM and only if
  // we walk the current stack!!! If we want to be able to walk
  // arbitrary other threads, we'll have to somehow store the thread
  // object in the frame.
  Thread *thread = Thread::current();
  if ((address)fr->fp() <=
      thread->register_stack_base() HPUX_ONLY(+ 0x0) LINUX_ONLY(+ 0x50)) {
    // This check is a little hacky, because on Linux the first C
    // frame's ('start_thread') register stack frame starts at
    // "register_stack_base + 0x48" while on HPUX, the first C frame's
    // ('__pthread_bound_body') register stack frame seems to really
    // start at "register_stack_base".
    return true;
  } else {
    return false;
  }
#elif defined(IA64) && defined(_WIN32)
  return true;
#else
  // Load up sp, fp, sender sp and sender fp, check for reasonable values.
  // Check usp first, because if that's bad the other accessors may fault
  // on some architectures.  Ditto ufp second, etc.
  uintptr_t fp_align_mask = (uintptr_t)(sizeof(address)-1);
  // sp on amd can be 32 bit aligned.
  uintptr_t sp_align_mask = (uintptr_t)(sizeof(int)-1);

  uintptr_t usp    = (uintptr_t)fr->sp();
  if ((usp & sp_align_mask) != 0) return true;

  uintptr_t ufp    = (uintptr_t)fr->fp();
  if ((ufp & fp_align_mask) != 0) return true;

  uintptr_t old_sp = (uintptr_t)fr->sender_sp();
  if ((old_sp & sp_align_mask) != 0) return true;
  if (old_sp == 0 || old_sp == (uintptr_t)-1) return true;

  uintptr_t old_fp = (uintptr_t)fr->link();
  if ((old_fp & fp_align_mask) != 0) return true;
  if (old_fp == 0 || old_fp == (uintptr_t)-1 || old_fp == ufp) return true;

  // stack grows downwards; if old_fp is below current fp or if the stack
  // frame is too large, either the stack is corrupted or fp is not saved
  // on stack (i.e. on x86, ebp may be used as general register). The stack
  // is not walkable beyond current frame.
  if (old_fp < ufp) return true;
  if (old_fp - ufp > 64 * K) return true;

  return false;
#endif
}

#ifdef ASSERT
extern "C" void test_random() {
  const double m = 2147483647;
  double mean = 0.0, variance = 0.0, t;
  long reps = 10000;
  unsigned long seed = 1;

  tty->print_cr("seed %ld for %ld repeats...", seed, reps);
  os::init_random(seed);
  long num;
  for (int k = 0; k < reps; k++) {
    num = os::random();
    double u = (double)num / m;
    assert(u >= 0.0 && u <= 1.0, "bad random number!");

    // calculate mean and variance of the random sequence
    mean += u;
    variance += (u*u);
  }
  mean /= reps;
  variance /= (reps - 1);

  assert(num == 1043618065, "bad seed");
  tty->print_cr("mean of the 1st 10000 numbers: %f", mean);
  tty->print_cr("variance of the 1st 10000 numbers: %f", variance);
  const double eps = 0.0001;
  t = fabsd(mean - 0.5018);
  assert(t < eps, "bad mean");
  t = (variance - 0.3355) < 0.0 ? -(variance - 0.3355) : variance - 0.3355;
  assert(t < eps, "bad variance");
}
#endif


// Set up the boot classpath.

char* os::format_boot_path(const char* format_string,
                           const char* home,
                           int home_len,
                           char fileSep,
                           char pathSep) {
    assert((fileSep == '/' && pathSep == ':') ||
           (fileSep == '\\' && pathSep == ';'), "unexpected separator chars");

    // Scan the format string to determine the length of the actual
    // boot classpath, and handle platform dependencies as well.
    int formatted_path_len = 0;
    const char* p;
    for (p = format_string; *p != 0; ++p) {
        if (*p == '%') formatted_path_len += home_len - 1;
        ++formatted_path_len;
    }

    char* formatted_path = NEW_C_HEAP_ARRAY(char, formatted_path_len + 1, mtInternal);
    if (formatted_path == NULL) {
        return NULL;
    }

    // Create boot classpath from format, substituting separator chars and
    // java home directory.
    char* q = formatted_path;
    for (p = format_string; *p != 0; ++p) {
        switch (*p) {
        case '%':
            strcpy(q, home);
            q += home_len;
            break;
        case '/':
            *q++ = fileSep;
            break;
        case ':':
            *q++ = pathSep;
            break;
        default:
            *q++ = *p;
        }
    }
    *q = '\0';

    assert((q - formatted_path) == formatted_path_len, "formatted_path size botched");
    return formatted_path;
}

bool os::set_boot_path(char fileSep, char pathSep) {
  const char* home = Arguments::get_java_home();
  int home_len = (int)strlen(home);

  struct stat st;

  // modular image if "modules" jimage exists
  char* jimage = format_boot_path("%/lib/" MODULES_IMAGE_NAME, home, home_len, fileSep, pathSep);
  if (jimage == NULL) return false;
  bool has_jimage = (os::stat(jimage, &st) == 0);
  if (has_jimage) {
    Arguments::set_sysclasspath(jimage, true);
    FREE_C_HEAP_ARRAY(char, jimage);
    return true;
  }
  FREE_C_HEAP_ARRAY(char, jimage);

  // check if developer build with exploded modules
  char* base_classes = format_boot_path("%/modules/java.base", home, home_len, fileSep, pathSep);
  if (base_classes == NULL) return false;
  if (os::stat(base_classes, &st) == 0) {
    Arguments::set_sysclasspath(base_classes, false);
    FREE_C_HEAP_ARRAY(char, base_classes);
    return true;
  }
  FREE_C_HEAP_ARRAY(char, base_classes);

  return false;
}

/*
 * Splits a path, based on its separator, the number of
 * elements is returned back in n.
 * It is the callers responsibility to:
 *   a> check the value of n, and n may be 0.
 *   b> ignore any empty path elements
 *   c> free up the data.
 */
char** os::split_path(const char* path, int* n) {
  *n = 0;
  if (path == NULL || strlen(path) == 0) {
    return NULL;
  }
  const char psepchar = *os::path_separator();
  char* inpath = (char*)NEW_C_HEAP_ARRAY(char, strlen(path) + 1, mtInternal);
  if (inpath == NULL) {
    return NULL;
  }
  strcpy(inpath, path);
  int count = 1;
  char* p = strchr(inpath, psepchar);
  // Get a count of elements to allocate memory
  while (p != NULL) {
    count++;
    p++;
    p = strchr(p, psepchar);
  }
  char** opath = (char**) NEW_C_HEAP_ARRAY(char*, count, mtInternal);
  if (opath == NULL) {
    return NULL;
  }

  // do the actual splitting
  p = inpath;
  for (int i = 0 ; i < count ; i++) {
    size_t len = strcspn(p, os::path_separator());
    if (len > JVM_MAXPATHLEN) {
      return NULL;
    }
    // allocate the string and add terminator storage
    char* s  = (char*)NEW_C_HEAP_ARRAY(char, len + 1, mtInternal);
    if (s == NULL) {
      return NULL;
    }
    strncpy(s, p, len);
    s[len] = '\0';
    opath[i] = s;
    p += len + 1;
  }
  FREE_C_HEAP_ARRAY(char, inpath);
  *n = count;
  return opath;
}

void os::set_memory_serialize_page(address page) {
  int count = log2_intptr(sizeof(class JavaThread)) - log2_intptr(64);
  _mem_serialize_page = (volatile int32_t *)page;
  // We initialize the serialization page shift count here
  // We assume a cache line size of 64 bytes
  assert(SerializePageShiftCount == count, "JavaThread size changed; "
         "SerializePageShiftCount constant should be %d", count);
  set_serialize_page_mask((uintptr_t)(vm_page_size() - sizeof(int32_t)));
}

static volatile intptr_t SerializePageLock = 0;

// This method is called from signal handler when SIGSEGV occurs while the current
// thread tries to store to the "read-only" memory serialize page during state
// transition.
void os::block_on_serialize_page_trap() {
  log_debug(safepoint)("Block until the serialize page permission restored");

  // When VMThread is holding the SerializePageLock during modifying the
  // access permission of the memory serialize page, the following call
  // will block until the permission of that page is restored to rw.
  // Generally, it is unsafe to manipulate locks in signal handlers, but in
  // this case, it's OK as the signal is synchronous and we know precisely when
  // it can occur.
  Thread::muxAcquire(&SerializePageLock, "set_memory_serialize_page");
  Thread::muxRelease(&SerializePageLock);
}

// Serialize all thread state variables
void os::serialize_thread_states() {
  // On some platforms such as Solaris & Linux, the time duration of the page
  // permission restoration is observed to be much longer than expected  due to
  // scheduler starvation problem etc. To avoid the long synchronization
  // time and expensive page trap spinning, 'SerializePageLock' is used to block
  // the mutator thread if such case is encountered. See bug 6546278 for details.
  Thread::muxAcquire(&SerializePageLock, "serialize_thread_states");
  os::protect_memory((char *)os::get_memory_serialize_page(),
                     os::vm_page_size(), MEM_PROT_READ);
  os::protect_memory((char *)os::get_memory_serialize_page(),
                     os::vm_page_size(), MEM_PROT_RW);
  Thread::muxRelease(&SerializePageLock);
}

// Returns true if the current stack pointer is above the stack shadow
// pages, false otherwise.
bool os::stack_shadow_pages_available(Thread *thread, const methodHandle& method, address sp) {
  if (!thread->is_Java_thread()) return false;
  // Check if we have StackShadowPages above the yellow zone.  This parameter
  // is dependent on the depth of the maximum VM call stack possible from
  // the handler for stack overflow.  'instanceof' in the stack overflow
  // handler or a println uses at least 8k stack of VM and native code
  // respectively.
  const int framesize_in_bytes =
    Interpreter::size_top_interpreter_activation(method()) * wordSize;

  address limit = ((JavaThread*)thread)->stack_end() +
                  (JavaThread::stack_guard_zone_size() + JavaThread::stack_shadow_zone_size());

  return sp > (limit + framesize_in_bytes);
}

size_t os::page_size_for_region(size_t region_size, size_t min_pages, bool must_be_aligned) {
  assert(min_pages > 0, "sanity");
  if (UseLargePages) {
    const size_t max_page_size = region_size / min_pages;

    for (size_t i = 0; _page_sizes[i] != 0; ++i) {
      const size_t page_size = _page_sizes[i];
      if (page_size <= max_page_size) {
        if (!must_be_aligned || is_size_aligned(region_size, page_size)) {
          return page_size;
        }
      }
    }
  }

  return vm_page_size();
}

size_t os::page_size_for_region_aligned(size_t region_size, size_t min_pages) {
  return page_size_for_region(region_size, min_pages, true);
}

size_t os::page_size_for_region_unaligned(size_t region_size, size_t min_pages) {
  return page_size_for_region(region_size, min_pages, false);
}

static const char* errno_to_string (int e, bool short_text) {
  #define ALL_SHARED_ENUMS(X) \
    X(E2BIG, "Argument list too long") \
    X(EACCES, "Permission denied") \
    X(EADDRINUSE, "Address in use") \
    X(EADDRNOTAVAIL, "Address not available") \
    X(EAFNOSUPPORT, "Address family not supported") \
    X(EAGAIN, "Resource unavailable, try again") \
    X(EALREADY, "Connection already in progress") \
    X(EBADF, "Bad file descriptor") \
    X(EBADMSG, "Bad message") \
    X(EBUSY, "Device or resource busy") \
    X(ECANCELED, "Operation canceled") \
    X(ECHILD, "No child processes") \
    X(ECONNABORTED, "Connection aborted") \
    X(ECONNREFUSED, "Connection refused") \
    X(ECONNRESET, "Connection reset") \
    X(EDEADLK, "Resource deadlock would occur") \
    X(EDESTADDRREQ, "Destination address required") \
    X(EDOM, "Mathematics argument out of domain of function") \
    X(EEXIST, "File exists") \
    X(EFAULT, "Bad address") \
    X(EFBIG, "File too large") \
    X(EHOSTUNREACH, "Host is unreachable") \
    X(EIDRM, "Identifier removed") \
    X(EILSEQ, "Illegal byte sequence") \
    X(EINPROGRESS, "Operation in progress") \
    X(EINTR, "Interrupted function") \
    X(EINVAL, "Invalid argument") \
    X(EIO, "I/O error") \
    X(EISCONN, "Socket is connected") \
    X(EISDIR, "Is a directory") \
    X(ELOOP, "Too many levels of symbolic links") \
    X(EMFILE, "Too many open files") \
    X(EMLINK, "Too many links") \
    X(EMSGSIZE, "Message too large") \
    X(ENAMETOOLONG, "Filename too long") \
    X(ENETDOWN, "Network is down") \
    X(ENETRESET, "Connection aborted by network") \
    X(ENETUNREACH, "Network unreachable") \
    X(ENFILE, "Too many files open in system") \
    X(ENOBUFS, "No buffer space available") \
    X(ENODATA, "No message is available on the STREAM head read queue") \
    X(ENODEV, "No such device") \
    X(ENOENT, "No such file or directory") \
    X(ENOEXEC, "Executable file format error") \
    X(ENOLCK, "No locks available") \
    X(ENOLINK, "Reserved") \
    X(ENOMEM, "Not enough space") \
    X(ENOMSG, "No message of the desired type") \
    X(ENOPROTOOPT, "Protocol not available") \
    X(ENOSPC, "No space left on device") \
    X(ENOSR, "No STREAM resources") \
    X(ENOSTR, "Not a STREAM") \
    X(ENOSYS, "Function not supported") \
    X(ENOTCONN, "The socket is not connected") \
    X(ENOTDIR, "Not a directory") \
    X(ENOTEMPTY, "Directory not empty") \
    X(ENOTSOCK, "Not a socket") \
    X(ENOTSUP, "Not supported") \
    X(ENOTTY, "Inappropriate I/O control operation") \
    X(ENXIO, "No such device or address") \
    X(EOPNOTSUPP, "Operation not supported on socket") \
    X(EOVERFLOW, "Value too large to be stored in data type") \
    X(EPERM, "Operation not permitted") \
    X(EPIPE, "Broken pipe") \
    X(EPROTO, "Protocol error") \
    X(EPROTONOSUPPORT, "Protocol not supported") \
    X(EPROTOTYPE, "Protocol wrong type for socket") \
    X(ERANGE, "Result too large") \
    X(EROFS, "Read-only file system") \
    X(ESPIPE, "Invalid seek") \
    X(ESRCH, "No such process") \
    X(ETIME, "Stream ioctl() timeout") \
    X(ETIMEDOUT, "Connection timed out") \
    X(ETXTBSY, "Text file busy") \
    X(EWOULDBLOCK, "Operation would block") \
    X(EXDEV, "Cross-device link")

  #define DEFINE_ENTRY(e, text) { e, #e, text },

  static const struct {
    int v;
    const char* short_text;
    const char* long_text;
  } table [] = {

    ALL_SHARED_ENUMS(DEFINE_ENTRY)

    // The following enums are not defined on all platforms.
    #ifdef ESTALE
    DEFINE_ENTRY(ESTALE, "Reserved")
    #endif
    #ifdef EDQUOT
    DEFINE_ENTRY(EDQUOT, "Reserved")
    #endif
    #ifdef EMULTIHOP
    DEFINE_ENTRY(EMULTIHOP, "Reserved")
    #endif

    // End marker.
    { -1, "Unknown errno", "Unknown error" }

  };

  #undef DEFINE_ENTRY
  #undef ALL_FLAGS

  int i = 0;
  while (table[i].v != -1 && table[i].v != e) {
    i ++;
  }

  return short_text ? table[i].short_text : table[i].long_text;

}

const char* os::strerror(int e) {
  return errno_to_string(e, false);
}

const char* os::errno_name(int e) {
  return errno_to_string(e, true);
}

void os::trace_page_sizes(const char* str, const size_t* page_sizes, int count) {
  LogTarget(Info, pagesize) log;
  if (log.is_enabled()) {
    LogStreamCHeap out(log);

    out.print("%s: ", str);
    for (int i = 0; i < count; ++i) {
      out.print(" " SIZE_FORMAT, page_sizes[i]);
    }
    out.cr();
  }
}

#define trace_page_size_params(size) byte_size_in_exact_unit(size), exact_unit_for_byte_size(size)

void os::trace_page_sizes(const char* str,
                          const size_t region_min_size,
                          const size_t region_max_size,
                          const size_t page_size,
                          const char* base,
                          const size_t size) {

  log_info(pagesize)("%s: "
                     " min=" SIZE_FORMAT "%s"
                     " max=" SIZE_FORMAT "%s"
                     " base=" PTR_FORMAT
                     " page_size=" SIZE_FORMAT "%s"
                     " size=" SIZE_FORMAT "%s",
                     str,
                     trace_page_size_params(region_min_size),
                     trace_page_size_params(region_max_size),
                     p2i(base),
                     trace_page_size_params(page_size),
                     trace_page_size_params(size));
}

void os::trace_page_sizes_for_requested_size(const char* str,
                                             const size_t requested_size,
                                             const size_t page_size,
                                             const size_t alignment,
                                             const char* base,
                                             const size_t size) {

  log_info(pagesize)("%s:"
                     " req_size=" SIZE_FORMAT "%s"
                     " base=" PTR_FORMAT
                     " page_size=" SIZE_FORMAT "%s"
                     " alignment=" SIZE_FORMAT "%s"
                     " size=" SIZE_FORMAT "%s",
                     str,
                     trace_page_size_params(requested_size),
                     p2i(base),
                     trace_page_size_params(page_size),
                     trace_page_size_params(alignment),
                     trace_page_size_params(size));
}


// This is the working definition of a server class machine:
// >= 2 physical CPU's and >=2GB of memory, with some fuzz
// because the graphics memory (?) sometimes masks physical memory.
// If you want to change the definition of a server class machine
// on some OS or platform, e.g., >=4GB on Windows platforms,
// then you'll have to parameterize this method based on that state,
// as was done for logical processors here, or replicate and
// specialize this method for each platform.  (Or fix os to have
// some inheritance structure and use subclassing.  Sigh.)
// If you want some platform to always or never behave as a server
// class machine, change the setting of AlwaysActAsServerClassMachine
// and NeverActAsServerClassMachine in globals*.hpp.
bool os::is_server_class_machine() {
  // First check for the early returns
  if (NeverActAsServerClassMachine) {
    return false;
  }
  if (AlwaysActAsServerClassMachine) {
    return true;
  }
  // Then actually look at the machine
  bool         result            = false;
  const unsigned int    server_processors = 2;
  const julong server_memory     = 2UL * G;
  // We seem not to get our full complement of memory.
  //     We allow some part (1/8?) of the memory to be "missing",
  //     based on the sizes of DIMMs, and maybe graphics cards.
  const julong missing_memory   = 256UL * M;

  /* Is this a server class machine? */
  if ((os::active_processor_count() >= (int)server_processors) &&
      (os::physical_memory() >= (server_memory - missing_memory))) {
    const unsigned int logical_processors =
      VM_Version::logical_processors_per_package();
    if (logical_processors > 1) {
      const unsigned int physical_packages =
        os::active_processor_count() / logical_processors;
      if (physical_packages >= server_processors) {
        result = true;
      }
    } else {
      result = true;
    }
  }
  return result;
}

void os::initialize_initial_active_processor_count() {
  assert(_initial_active_processor_count == 0, "Initial active processor count already set.");
  _initial_active_processor_count = active_processor_count();
  log_debug(os)("Initial active processor count set to %d" , _initial_active_processor_count);
}

void os::SuspendedThreadTask::run() {
  assert(Threads_lock->owned_by_self() || (_thread == VMThread::vm_thread()), "must have threads lock to call this");
  internal_do_task();
  _done = true;
}

bool os::create_stack_guard_pages(char* addr, size_t bytes) {
  return os::pd_create_stack_guard_pages(addr, bytes);
}

char* os::reserve_memory(size_t bytes, char* addr, size_t alignment_hint) {
  char* result = pd_reserve_memory(bytes, addr, alignment_hint);
  if (result != NULL) {
    MemTracker::record_virtual_memory_reserve((address)result, bytes, CALLER_PC);
  }

  return result;
}

char* os::reserve_memory(size_t bytes, char* addr, size_t alignment_hint,
   MEMFLAGS flags) {
  char* result = pd_reserve_memory(bytes, addr, alignment_hint);
  if (result != NULL) {
    MemTracker::record_virtual_memory_reserve((address)result, bytes, CALLER_PC);
    MemTracker::record_virtual_memory_type((address)result, flags);
  }

  return result;
}

char* os::attempt_reserve_memory_at(size_t bytes, char* addr) {
  char* result = pd_attempt_reserve_memory_at(bytes, addr);
  if (result != NULL) {
    MemTracker::record_virtual_memory_reserve((address)result, bytes, CALLER_PC);
  }
  return result;
}

void os::split_reserved_memory(char *base, size_t size,
                                 size_t split, bool realloc) {
  pd_split_reserved_memory(base, size, split, realloc);
}

bool os::commit_memory(char* addr, size_t bytes, bool executable) {
  bool res = pd_commit_memory(addr, bytes, executable);
  if (res) {
    MemTracker::record_virtual_memory_commit((address)addr, bytes, CALLER_PC);
  }
  return res;
}

bool os::commit_memory(char* addr, size_t size, size_t alignment_hint,
                              bool executable) {
  bool res = os::pd_commit_memory(addr, size, alignment_hint, executable);
  if (res) {
    MemTracker::record_virtual_memory_commit((address)addr, size, CALLER_PC);
  }
  return res;
}

void os::commit_memory_or_exit(char* addr, size_t bytes, bool executable,
                               const char* mesg) {
  pd_commit_memory_or_exit(addr, bytes, executable, mesg);
  MemTracker::record_virtual_memory_commit((address)addr, bytes, CALLER_PC);
}

void os::commit_memory_or_exit(char* addr, size_t size, size_t alignment_hint,
                               bool executable, const char* mesg) {
  os::pd_commit_memory_or_exit(addr, size, alignment_hint, executable, mesg);
  MemTracker::record_virtual_memory_commit((address)addr, size, CALLER_PC);
}

bool os::uncommit_memory(char* addr, size_t bytes) {
  bool res;
  if (MemTracker::tracking_level() > NMT_minimal) {
    Tracker tkr = MemTracker::get_virtual_memory_uncommit_tracker();
    res = pd_uncommit_memory(addr, bytes);
    if (res) {
      tkr.record((address)addr, bytes);
    }
  } else {
    res = pd_uncommit_memory(addr, bytes);
  }
  return res;
}

bool os::release_memory(char* addr, size_t bytes) {
  bool res;
  if (MemTracker::tracking_level() > NMT_minimal) {
    Tracker tkr = MemTracker::get_virtual_memory_release_tracker();
    res = pd_release_memory(addr, bytes);
    if (res) {
      tkr.record((address)addr, bytes);
    }
  } else {
    res = pd_release_memory(addr, bytes);
  }
  return res;
}

void os::pretouch_memory(void* start, void* end) {
  for (volatile char *p = (char*)start; p < (char*)end; p += os::vm_page_size()) {
    *p = 0;
  }
}

char* os::map_memory(int fd, const char* file_name, size_t file_offset,
                           char *addr, size_t bytes, bool read_only,
                           bool allow_exec) {
  char* result = pd_map_memory(fd, file_name, file_offset, addr, bytes, read_only, allow_exec);
  if (result != NULL) {
    MemTracker::record_virtual_memory_reserve_and_commit((address)result, bytes, CALLER_PC);
  }
  return result;
}

char* os::remap_memory(int fd, const char* file_name, size_t file_offset,
                             char *addr, size_t bytes, bool read_only,
                             bool allow_exec) {
  return pd_remap_memory(fd, file_name, file_offset, addr, bytes,
                    read_only, allow_exec);
}

bool os::unmap_memory(char *addr, size_t bytes) {
  bool result;
  if (MemTracker::tracking_level() > NMT_minimal) {
    Tracker tkr = MemTracker::get_virtual_memory_release_tracker();
    result = pd_unmap_memory(addr, bytes);
    if (result) {
      tkr.record((address)addr, bytes);
    }
  } else {
    result = pd_unmap_memory(addr, bytes);
  }
  return result;
}

void os::free_memory(char *addr, size_t bytes, size_t alignment_hint) {
  pd_free_memory(addr, bytes, alignment_hint);
}

void os::realign_memory(char *addr, size_t bytes, size_t alignment_hint) {
  pd_realign_memory(addr, bytes, alignment_hint);
}

#ifndef _WINDOWS
/* try to switch state from state "from" to state "to"
 * returns the state set after the method is complete
 */
os::SuspendResume::State os::SuspendResume::switch_state(os::SuspendResume::State from,
                                                         os::SuspendResume::State to)
{
  os::SuspendResume::State result =
    (os::SuspendResume::State) Atomic::cmpxchg((jint) to, (jint *) &_state, (jint) from);
  if (result == from) {
    // success
    return to;
  }
  return result;
}
#endif

/////////////// Unit tests ///////////////

#ifndef PRODUCT

#define assert_eq(a,b) assert(a == b, SIZE_FORMAT " != " SIZE_FORMAT, a, b)

class TestOS : AllStatic {
  static size_t small_page_size() {
    return os::vm_page_size();
  }

  static size_t large_page_size() {
    const size_t large_page_size_example = 4 * M;
    return os::page_size_for_region_aligned(large_page_size_example, 1);
  }

  static void test_page_size_for_region_aligned() {
    if (UseLargePages) {
      const size_t small_page = small_page_size();
      const size_t large_page = large_page_size();

      if (large_page > small_page) {
        size_t num_small_pages_in_large = large_page / small_page;
        size_t page = os::page_size_for_region_aligned(large_page, num_small_pages_in_large);

        assert_eq(page, small_page);
      }
    }
  }

  static void test_page_size_for_region_alignment() {
    if (UseLargePages) {
      const size_t small_page = small_page_size();
      const size_t large_page = large_page_size();
      if (large_page > small_page) {
        const size_t unaligned_region = large_page + 17;
        size_t page = os::page_size_for_region_aligned(unaligned_region, 1);
        assert_eq(page, small_page);

        const size_t num_pages = 5;
        const size_t aligned_region = large_page * num_pages;
        page = os::page_size_for_region_aligned(aligned_region, num_pages);
        assert_eq(page, large_page);
      }
    }
  }

  static void test_page_size_for_region_unaligned() {
    if (UseLargePages) {
      // Given exact page size, should return that page size.
      for (size_t i = 0; os::_page_sizes[i] != 0; i++) {
        size_t expected = os::_page_sizes[i];
        size_t actual = os::page_size_for_region_unaligned(expected, 1);
        assert_eq(expected, actual);
      }

      // Given slightly larger size than a page size, return the page size.
      for (size_t i = 0; os::_page_sizes[i] != 0; i++) {
        size_t expected = os::_page_sizes[i];
        size_t actual = os::page_size_for_region_unaligned(expected + 17, 1);
        assert_eq(expected, actual);
      }

      // Given a slightly smaller size than a page size,
      // return the next smaller page size.
      if (os::_page_sizes[1] > os::_page_sizes[0]) {
        size_t expected = os::_page_sizes[0];
        size_t actual = os::page_size_for_region_unaligned(os::_page_sizes[1] - 17, 1);
        assert_eq(actual, expected);
      }

      // Return small page size for values less than a small page.
      size_t small_page = small_page_size();
      size_t actual = os::page_size_for_region_unaligned(small_page - 17, 1);
      assert_eq(small_page, actual);
    }
  }

 public:
  static void run_tests() {
    test_page_size_for_region_aligned();
    test_page_size_for_region_alignment();
    test_page_size_for_region_unaligned();
  }
};

void TestOS_test() {
  TestOS::run_tests();
}

#endif // PRODUCT
