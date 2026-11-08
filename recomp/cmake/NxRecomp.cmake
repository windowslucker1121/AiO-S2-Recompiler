# SPDX-License-Identifier: GPL-3.0-or-later
#
# nxrecomp_add_module(<target>
#     INPUT <module file>          NSO or static ELF (see --kind)
#     NAME <module name>           default: main        (the generated descriptor is `nxmod_<NAME>`)
#     [BASE <0x...>]               guest address the module is mapped at (default 0x7100000000)
#     [INDEX <n>]                  NxModule.index / import-id module number (default: position in the game config's module list)
#     [GAME <name>]                recomp/games/<name>/config.py (default: generic)
#     [KIND nso|nx_elf|static_elf] default: guessed from the file's magic
#     [SHARDS <n>]                 exactly n translation units c_000.cpp ... (default 16)
#     [JOBS <n>]                   lifter processes (default: all cores)
#     [PATCHES <file>]             instruction patch list
#     [SITES <file>]               site list: patches, inline callouts, function hooks (nxrecomp/program.py load_sites)
#     [OUTDIR <dir>])              where the generated sources go (default: <build>/gen/<target>)
#
# Lifts the module with `python -m nxrecomp lift` (a build step, re-run when the input or the lifter changes) and builds
# the result as the static library <target>. Link it into the runtime: it provides `nxmod_<NAME>` (ctx.h: NxModule) and
# expects the runtime to provide the functions ctx.h declares as runtime-provided. Public include directories: recomp/include
# (nxrecomp/ctx.h) and the generated folder (funcs.h).
#
# Flags: clang-cl, C++20, no exceptions/RTTI, -fno-strict-aliasing and -ffp-contract=off (smo-recomp's: lifted FP must not be
# fused or reordered), -mcx16 (128-bit exclusives are cmpxchg16b), NX_LIFT_OPT (/O2) and NX_ISA (-march=x86-64-v3, or
# baseline). Functions longer than the game config's optnone_insns are emitted with NX_OPTNONE by the lifter itself (bounded
# compile time); cmake/nx_cc_launcher.py adds the time-limit fallback.

set(NXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "recomp/ folder")
set(NX_PYTHON "${NXRECOMP_ROOT}/../tools/.venv/Scripts/python.exe" CACHE FILEPATH "Python with the nxrecomp requirements")
# (NX_LIFT_OPT, not the earlier NX_OPT: build folders configured before keep NX_OPT=/O1 in their cache)
set(NX_LIFT_OPT "/O2" CACHE STRING "optimisation flag of the lifted code (clang-cl syntax)")
# Target ISA of the lifted code. x86-64-v3 (AVX2, FMA3, BMI1/2, LZCNT, MOVBE; Haswell / Zen 1 and later) turns the exact
# fmaf / fma of FMADD and FMLA into one vfmadd and FRINT* into vroundss instead of C library calls. -ffp-contract=off stays,
# so separate FMUL + FADD are never fused and results are bit-identical to the baseline build. Release builds for players
# whose CPUs lack AVX2 configure with -DNX_ISA=baseline (plain x86-64, SSE2; scripts/build_game.cmd passes the environment
# variable NX_ISA through).
set(NX_ISA "x86-64-v3" CACHE STRING "target ISA of the lifted code: x86-64-v3 (default) or baseline (any x86-64 CPU)")
set_property(CACHE NX_ISA PROPERTY STRINGS x86-64-v3 x86-64-v2 baseline)
if(NX_ISA STREQUAL "baseline")
  set(NX_ISA_FLAGS "")
else()
  set(NX_ISA_FLAGS "-march=${NX_ISA}")
endif()
option(NX_USE_LAUNCHER "wrap lifted-code compiles in cmake/nx_cc_launcher.py (time-limit fallback to optnone / -O0)" OFF)

file(GLOB NX_LIFTER_SOURCES CONFIGURE_DEPENDS
     "${NXRECOMP_ROOT}/nxrecomp/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/tier2/*.py"
     "${NXRECOMP_ROOT}/include/nxrecomp/*.h")

function(nxrecomp_add_module target)
  cmake_parse_arguments(A "" "INPUT;NAME;BASE;INDEX;GAME;KIND;SHARDS;JOBS;OUTDIR;PATCHES;SITES" "" ${ARGN})
  if(NOT A_NAME)
    set(A_NAME main)
  endif()
  if(NOT A_BASE)
    set(A_BASE 0x7100000000)
  endif()
  if(NOT A_SHARDS)
    set(A_SHARDS 16)
  endif()
  if(NOT A_JOBS)
    cmake_host_system_information(RESULT A_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  endif()
  if(NOT A_OUTDIR)
    set(A_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  endif()
  set(extra "")
  set(game_deps "")
  if(A_GAME)
    list(APPEND extra --game ${A_GAME})
    file(GLOB game_deps CONFIGURE_DEPENDS "${NXRECOMP_ROOT}/games/${A_GAME}/*.py")
  endif()
  if(A_KIND)
    list(APPEND extra --kind ${A_KIND})
  endif()
  if(NOT "${A_INDEX}" STREQUAL "")
    list(APPEND extra --index ${A_INDEX})
  endif()
  if(A_PATCHES)
    list(APPEND extra --patches ${A_PATCHES})
  endif()
  if(A_SITES)
    list(APPEND extra --sites ${A_SITES})
  endif()
  set(outs "${A_OUTDIR}/funcs.h" "${A_OUTDIR}/module_${A_NAME}.cpp")
  set(shard_srcs "")
  math(EXPR last "${A_SHARDS} - 1")
  foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
      set(n "00${i}")
    elseif(len EQUAL 2)
      set(n "0${i}")
    else()
      set(n "${i}")
    endif()
    list(APPEND shard_srcs "${A_OUTDIR}/c_${n}.cpp")
  endforeach()
  add_custom_command(
    OUTPUT ${outs} ${shard_srcs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${A_OUTDIR}"
    COMMAND "${NX_PYTHON}" -m nxrecomp lift "${A_INPUT}" "${A_OUTDIR}" --name ${A_NAME} --base ${A_BASE}
            --shards ${A_SHARDS} --jobs ${A_JOBS} ${extra}
    WORKING_DIRECTORY "${NXRECOMP_ROOT}"
    DEPENDS "${A_INPUT}" ${A_SITES} ${NX_LIFTER_SOURCES} ${game_deps}
    COMMENT "nxrecomp: lifting ${A_NAME} (${A_INPUT})"
    VERBATIM)
  add_library(${target} STATIC ${shard_srcs} "${A_OUTDIR}/module_${A_NAME}.cpp")
  target_include_directories(${target} PUBLIC "${NXRECOMP_ROOT}/include" "${A_OUTDIR}")
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(MSVC)   # clang-cl
    # /GS-: no stack cookies (the guest has its own stack; host frames of lifted code hold no buffers to protect);
    # -fno-math-errno: sqrtf of a negative number stays one sqrtss (no errno call; the lifted code never reads errno)
    target_compile_options(${target} PRIVATE ${NX_LIFT_OPT} ${NX_ISA_FLAGS} /w /EHs-c- /GR- /GS- -fno-strict-aliasing
                           -ffp-contract=off -fno-math-errno -mcx16)
  else()
    target_compile_options(${target} PRIVATE -O2 ${NX_ISA_FLAGS} -w -fno-exceptions -fno-rtti -fno-strict-aliasing
                           -ffp-contract=off -fno-math-errno -mcx16)
  endif()
  if(NX_USE_LAUNCHER)
    set_target_properties(${target} PROPERTIES CXX_COMPILER_LAUNCHER "${NX_PYTHON};${NXRECOMP_ROOT}/cmake/nx_cc_launcher.py")
  endif()
endfunction()
