// SPDX-License-Identifier: GPL-3.0-or-later
// C++ exception support for lifted code: the unwinder (the runtime IS _Unwind_RaiseException / _Unwind_Resume), Itanium-ABI typeinfo
// matching (__do_catch semantics) and the catch frames behind nx_catch_call. docs/S3b.md describes the whole boundary.
//
// What is guest code and what is runtime
//   guest (recompiled libc++abi): __cxa_allocate_exception, __cxa_throw, __cxa_begin_catch, __cxa_end_catch, __cxa_rethrow,
//       std::terminate, the exception header (__cxa_exception) and the per-thread caught-exception stack.
//   runtime (this header): _Unwind_RaiseException, _Unwind_Resume, _Unwind_Resume_or_Rethrow, _Unwind_DeleteException (handlers
//       below), the two-phase search/cleanup walk of the guest stack over NxModule.eh, the personality routine's typeinfo
//       matching, landing (x0 = _Unwind_Exception*, x1 = selector, callee-saved registers and SP of the landing frame), and
//       nx_catch_call's setjmp frames. The guest's own libunwind/personality (__gxx_personality_v0) is never called.
// The guest reaches the handlers in three ways, all equivalent: an HLE thunk by import name ("_Unwind_RaiseException", registered
// by register_unwind_hle()), an entry hook on the guest function (hook_add_entry + hook_unwind_*), or a lifter function override
// (game config `overrides`: guest address -> "nxrt_unwind_raise_exception", which the runtime exports as the extern "C" functions
// h_nxrt_unwind_raise_exception etc. in src/eh/handlers.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt::eh {

// ---- typeinfo ----------------------------------------------------------------------------------------------------------------
// The guest's std::type_info objects are read in place (guest pointers are host pointers). Their kind is told by the vtable pointer:
// a typeinfo of kind K has vptr == (address of the vtable symbol of K) + 16. The addresses of those symbols are configured here.
struct TypeinfoVtables {
    uint64_t fundamental = 0;          // _ZTVN10__cxxabiv123__fundamental_type_infoE
    uint64_t array = 0;                // _ZTVN10__cxxabiv117__array_type_infoE
    uint64_t function = 0;             // _ZTVN10__cxxabiv120__function_type_infoE
    uint64_t enumeration = 0;          // _ZTVN10__cxxabiv116__enum_type_infoE
    uint64_t klass = 0;                // _ZTVN10__cxxabiv117__class_type_infoE
    uint64_t si_class = 0;             // _ZTVN10__cxxabiv120__si_class_type_infoE
    uint64_t vmi_class = 0;            // _ZTVN10__cxxabiv121__vmi_class_type_infoE
    uint64_t pointer = 0;              // _ZTVN10__cxxabiv119__pointer_type_infoE
    uint64_t pointer_to_member = 0;    // _ZTVN10__cxxabiv129__pointer_to_member_type_infoE
};
void set_typeinfo_vtables(const TypeinfoVtables& v);
const TypeinfoVtables& typeinfo_vtables();
// Looks the nine symbols up with module_find_symbol and nxmodule_find_export, keeps what it finds, returns how many were found.
// Run it after the modules are linked. A kind without a symbol is never recognised (its typeinfos match only themselves).
size_t discover_typeinfo_vtables();

enum class TiKind : uint8_t { Unknown, Fundamental, Array, Function, Enum, Class, SiClass, VmiClass, Pointer, PointerToMember };
TiKind ti_kind(uint64_t ti);
inline bool ti_is_class(TiKind k) { return k == TiKind::Class || k == TiKind::SiClass || k == TiKind::VmiClass; }
const char* ti_name(uint64_t ti);                 // mangled name, "" for null/unreadable
bool ti_equal(uint64_t a, uint64_t b);            // same object, or same name (names starting with '*' have internal linkage: object only)
std::string ti_demangle_hint(uint64_t ti);        // "name (kind)" for logs

// The personality routine's test of one handler. `catch_ti` is the typeinfo of the handler (0 = catch(...)), `thrown_ti` the type of
// the exception object. `*adjusted` is the address of the thrown object on entry (for a thrown pointer: the address of the pointer)
// and, on success, the value to hand to the handler (the base subobject for a derived-to-base catch; the pointer value, converted,
// for a pointer catch): this is what __cxa_begin_catch returns. Mirrors __pbase/__pointer/__class_type_info::can_catch.
bool can_catch(uint64_t catch_ti, uint64_t thrown_ti, uint64_t* adjusted);

// ---- the exception header of the guest's libc++abi ---------------------------------------------------------------------------
// Offsets from the _Unwind_Exception that _Unwind_RaiseException receives. __cxa_exception is 0x80 bytes, the _Unwind_Exception at +0x60
// and the thrown object right behind it in every libc++abi we know; the fields in front of it moved by 8 bytes between two LLVM
// versions: newer libc++abi starts the struct with `void* reserve` (exceptionType at -0x50), the older one, which the SDK of ssbu-recomp's
// game has (refs/ssbu-recomp/runtime/eh_runtime.h), has no such field and pads in front of the unwind header instead (exceptionType at -0x58).
// By default the runtime tells the two apart at the first exception: the one whose exceptionType points to a readable typeinfo wins and is kept.
// Fields are defaults of the newer layout; abi_layout_llvm_old() is the other. To be confirmed against the dump.
struct AbiLayout {
    int64_t exception_type = -0x50;       // std::type_info* exceptionType
    int64_t handler_switch = -0x24;       // int handlerSwitchValue                       (written when a handler is found)
    int64_t action_record = -0x20;        // const unsigned char* actionRecord            (written as 0)
    int64_t lsda = -0x18;                 // const unsigned char* languageSpecificData    (written as 0)
    int64_t catch_temp = -0x10;           // void* catchTemp = landing pad                (written when a handler is found)
    int64_t adjusted_ptr = -0x8;          // void* adjustedPtr                            (written when a handler is found)
    int64_t primary = -0x58;              // __cxa_dependent_exception::primaryException = the primary's thrown OBJECT (dependent only)
    int64_t object = 0x20;                // the thrown object
    int64_t object_type = -0x70;          // from a thrown object to its own std::type_info* exceptionType (used for a primary exception)
    uint64_t class_mask = 0xFFFFFFFFFFFFFF00ull;   // vendor and language bits of exception_class
    uint64_t class_clang = 0x434C4E47432B2B00ull;  // "CLNGC++\0"  (low byte 1 = dependent)
    uint64_t class_gnu = 0x474E5543432B2B00ull;    // "GNUCC++\0"
};
AbiLayout abi_layout_llvm_new();                  // reserve word first
AbiLayout abi_layout_llvm_old();                  // no reserve word
void set_abi_layout(const AbiLayout& l);          // fixes the layout (no detection)
void set_abi_autodetect();                        // forget the layout and detect it again at the next exception (the default state)
const AbiLayout& abi_layout();                    // the layout in use (the newer one until an exception has been seen)

// ---- the unwinder ------------------------------------------------------------------------------------------------------------
// What is being thrown. The libc++abi front end (unwind_raise_exception) builds it from the _Unwind_Exception; the host-service
// front end of the S1 end-to-end programs (oracle/e2e/progs/rt.h: raise(object, typeinfo) / resume(object)) builds it directly.
struct Thrown {
    uint64_t exc = 0;          // handed back to the landing pad in x0 (an _Unwind_Exception*, or the bare object in the legacy form)
    uint64_t type = 0;         // typeinfo of the thrown type; 0 = foreign exception (only catch(...) and cleanups act)
    uint64_t object = 0;       // address of the thrown object (the initial adjustedPtr)
    uint64_t header = 0;       // the _Unwind_Exception whose __cxa_exception fields get the handler info, 0 = write nothing
};

// _URC_* results of _Unwind_RaiseException when it returns (it returns only when it fails).
constexpr uint64_t kUrcEndOfStack = 5;
constexpr uint64_t kUrcFatalPhase1Error = 3;
constexpr uint64_t kUrcFatalPhase2Error = 2;

// Two-phase raise starting at the caller of the HLE function (c->x[30] is the return address into the guest). On success this does not
// return (it lands in a catch frame or a cleanup pad). When no handler exists it logs the guest backtrace and returns kUrcEndOfStack,
// leaving the guest stack and registers untouched (libgcc/libc++abi behaviour: __cxa_throw then calls std::terminate).
uint64_t raise(::Ctx* c, const Thrown& t);
// Continue after a cleanup pad (_Unwind_Resume): starts at the caller of the cleanup function. Does not return unless the exception
// finds no handler (then it behaves like raise: logs, returns kUrcEndOfStack... a resume that returns is a guest error).
uint64_t resume(::Ctx* c, const Thrown& t);

// _Unwind_Exception front ends: x0 = _Unwind_Exception*; result in x0 when they return.
void unwind_raise_exception(::Ctx* c);
void unwind_resume(::Ctx* c);
void unwind_resume_or_rethrow(::Ctx* c);
void unwind_delete_exception(::Ctx* c);          // calls exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, ue) if set
void unwind_forced_unwind(::Ctx* c);             // unsupported: logs once, returns kUrcFatalPhase1Error

// Registers the front ends as HLE handlers under their libgcc/libunwind names (_Unwind_RaiseException, _Unwind_Resume,
// _Unwind_Resume_or_Rethrow, _Unwind_DeleteException, _Unwind_ForcedUnwind). Idempotent. Call before module_link_all() when an
// import by those names must reach the runtime (module_override_symbol is applied for the first four).
void register_unwind_hle();
// Entry-hook functions (hooks.h SiteHookFn shape) for the guest functions that implement the same API in the recompiled program:
// hook_add_entry(addr_of__Unwind_RaiseException, hook_unwind_raise_exception, nullptr), etc. They run the handler and return 1.
int hook_unwind_raise_exception(::Ctx* c, uint64_t addr, void* user);
int hook_unwind_resume(::Ctx* c, uint64_t addr, void* user);
int hook_unwind_resume_or_rethrow(::Ctx* c, uint64_t addr, void* user);
int hook_unwind_delete_exception(::Ctx* c, uint64_t addr, void* user);

// The S1 end-to-end programs' form (progs/rt.h): x0 = object, x1 = typeinfo. A missing handler is std::terminate here (the guest
// has no way to call it): terminate_process().
void legacy_raise(::Ctx* c);
void legacy_resume(::Ctx* c);

// ---- catch frames ------------------------------------------------------------------------------------------------------------
// nx_catch_call (ctx.h) is defined in src/eh/catch_call.cpp on top of these; they are exposed for tests and diagnostics.
size_t catch_frame_depth();                       // catch frames live on the calling thread
// A guest longjmp branching to `addr` (a setjmp return address, made a landing pad by the lifter): resumes the setjmp caller's
// catch frame there and does not return. False when addr is no landing pad or no live catch frame lies above the guest SP.
bool longjmp_landing(::Ctx* c, uint64_t addr);

// ---- terminate and diagnostics -----------------------------------------------------------------------------------------------
// Guest backtrace from the registers in `c` (pc = c->x[30], then the CFI chain): "#0 0x... module+off (symbol+off)" lines.
std::string guest_backtrace(const ::Ctx* c, size_t max_frames = 64);
// Logs "std::terminate: <why>" with the guest backtrace, flushes the log and ends the process with exit code kTerminateExitCode.
// A handler set with set_terminate_handler() runs instead of ending the process (tests); if it returns, so does terminate_process.
constexpr int kTerminateExitCode = 134;
using TerminateHandler = void (*)(::Ctx* c, const char* why);
void set_terminate_handler(TerminateHandler h);
void terminate_process(::Ctx* c, const char* why);
// Counters for the end-of-run summary and tests.
struct Stats {
    uint64_t raised = 0, resumed = 0, caught = 0, cleanups = 0, uncaught = 0, frames_walked = 0, catch_frame_landings = 0, resume_fn_landings = 0;
};
Stats stats();
void stats_reset();

}  // namespace nxrt::eh

extern "C" {
// Lifter function overrides (game config `overrides = {addr: "nxrt_unwind_raise_exception"}` -> h_nxrt_unwind_raise_exception(Ctx *c)).
void h_nxrt_unwind_raise_exception(Ctx* c);
void h_nxrt_unwind_resume(Ctx* c);
void h_nxrt_unwind_resume_or_rethrow(Ctx* c);
void h_nxrt_unwind_delete_exception(Ctx* c);
}
