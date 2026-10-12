# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Build one Rust host archive containing config, sql-nio and optional plugin
# runtime. sql_nio and config expose compatible C++ interface targets.
#
# Usage from any C++ target:
#     target_link_libraries(<your_target> PRIVATE sql_nio)
#     #include "nio.h"
#     target_link_libraries(<your_target> PRIVATE config)
#     #include "config.h"
#
# Override the workspace location by setting RUST_WORKSPACE_DIR before include().

if(NOT DEFINED RUST_WORKSPACE_DIR)
  set(RUST_WORKSPACE_DIR "${CMAKE_SOURCE_DIR}/rust")
endif()
set(SQL_NIO_CRATE_DIR   "${RUST_WORKSPACE_DIR}/sql-nio")
set(SQL_NIO_INCLUDE_DIR "${SQL_NIO_CRATE_DIR}/include")
set(CONFIG_CRATE_DIR "${RUST_WORKSPACE_DIR}/config")
set(CONFIG_INCLUDE_DIR "${CONFIG_CRATE_DIR}/include")

# Locate cargo: explicit -DCARGO=, else PATH, else the rustup default location.
if(NOT CARGO)
  find_program(CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
endif()
if(NOT CARGO)
  message(FATAL_ERROR "[rust] cargo not found. Install via https://rustup.rs, "
                      "or pass -DCARGO=/path/to/cargo.")
endif()
message(STATUS "[rust] cargo: ${CARGO}")

# Map the CMake build type to a cargo profile and its output subdirectory.
# Debug uses the dedicated cmake-debug profile (dev codegen + panic="abort")
# rather than plain dev, so no linked build can unwind a panic into C++.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_cargo_profile_flag "--profile" "cmake-debug")
  set(_cargo_out_subdir "cmake-debug")
else()
  set(_cargo_profile_flag "--release")
  set(_cargo_out_subdir "release")
endif()

# Cargo otherwise builds for the host during a mobile CMake cross-build.
# Keep the Rust static library on the same target and API level as the C++ code.
set(_cargo_target_args)
set(_cargo_target_subdir)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(_rust_target_triple "${SEEKDB_IOS_RUST_TARGET}")
  list(APPEND _cargo_target_args "--target" "${_rust_target_triple}")
  set(_cargo_target_subdir "${_rust_target_triple}/")
elseif(ANDROID)
  if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "[rust] unsupported Android ABI: ${CMAKE_ANDROID_ARCH_ABI}")
  endif()
  set(_rust_target_triple "aarch64-linux-android")
  list(APPEND _cargo_target_args "--target" "${_rust_target_triple}")
  set(_cargo_target_subdir "${_rust_target_triple}/")
endif()

# Keep all cargo output inside the CMake build tree (isolated per build dir).
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target" CACHE PATH
    "Cargo output directory; may reuse a previously built target directory")
# Cargo's aggregate staticlib artifact name is platform-specific.
if(WIN32)
  set(RUST_STATICLIB "${RUST_TARGET_DIR}/${_cargo_out_subdir}/seekdb_host.lib")
else()
  set(RUST_STATICLIB "${RUST_TARGET_DIR}/${_cargo_target_subdir}${_cargo_out_subdir}/libseekdb_host.a")
endif()

# Reinvoke Cargo when any workspace crate's source or build input changes.
# Discover crates through their manifests so rust/target is never scanned.
file(GLOB _rust_inputs CONFIGURE_DEPENDS
  "${RUST_WORKSPACE_DIR}/*.toml"
  "${RUST_WORKSPACE_DIR}/*.lock")
file(GLOB _rust_crate_manifests CONFIGURE_DEPENDS
  "${RUST_WORKSPACE_DIR}/*/Cargo.toml")
foreach(_rust_manifest IN LISTS _rust_crate_manifests)
  get_filename_component(_rust_crate_dir "${_rust_manifest}" DIRECTORY)
  file(GLOB_RECURSE _rust_crate_sources CONFIGURE_DEPENDS
    "${_rust_crate_dir}/src/*.rs")
  file(GLOB _rust_crate_build_inputs CONFIGURE_DEPENDS
    "${_rust_crate_dir}/*.toml"
    "${_rust_crate_dir}/*.rs"
    "${_rust_crate_dir}/*.yaml"
    "${_rust_crate_dir}/*.yml"
    "${_rust_crate_dir}/*.json")
  list(APPEND _rust_inputs ${_rust_crate_sources} ${_rust_crate_build_inputs})
endforeach()

set(_rust_host_features)
if(SEEKDB_ENABLE_EXPERIMENTAL_PLUGINS)
  list(APPEND _rust_host_features --features plugins)
endif()
# Make generators must also rerun Cargo when only the feature option changes.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/seekdb-rust-host-features.txt"
  CONTENT "${_rust_host_features}\n")
list(APPEND _rust_inputs "${CMAKE_CURRENT_BINARY_DIR}/seekdb-rust-host-features.txt")

# CC/AR: cargo inherits CMake's PATH but not its compiler variables, and
# `ring` (rustls's crypto backend) compiles C through the `cc` crate. Pin it
# to the same toolchain as the rest of the build instead of whatever `cc`
# discovers on PATH.
set(_rust_build_env "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
                    "CONFIG_HEADER_DIR=${RUST_TARGET_DIR}/include"
                    "DEFAULT_LOG_LEVEL=${DEFAULT_LOG_LEVEL}"
                    "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
                    "AR=${CMAKE_AR}")
if(ANDROID)
  string(REGEX REPLACE "^android-" "" _android_api "${ANDROID_PLATFORM}")
  get_filename_component(_ndk_toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
  set(_android_clang "${_ndk_toolchain_bin}/aarch64-linux-android${_android_api}-clang")
  set(_android_ar "${_ndk_toolchain_bin}/llvm-ar")
  if(NOT EXISTS "${_android_clang}")
    message(FATAL_ERROR "[rust] Android clang wrapper not found: ${_android_clang}")
  endif()
  list(APPEND _rust_build_env
       "CC_aarch64_linux_android=${_android_clang}"
       "AR_aarch64_linux_android=${_android_ar}"
       "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=${_android_clang}")
endif()
if(WIN32)
  # rustup treats an exact-version override and the stable alias as distinct
  # installed toolchains, even when stable currently is that exact version.
  # Reuse the installed alias only after proving that it satisfies our pin;
  # this avoids a needless network sync without weakening reproducibility.
  file(STRINGS "${RUST_WORKSPACE_DIR}/rust-toolchain.toml" _rust_channel_line
       REGEX "^[ \t]*channel[ \t]*=[ \t]*\"[0-9]+\\.[0-9]+\\.[0-9]+\"")
  string(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1"
         _rust_pinned_version "${_rust_channel_line}")
  find_program(RUSTUP rustup HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin")
  if(RUSTUP AND _rust_pinned_version)
    execute_process(
      COMMAND "${RUSTUP}" run stable rustc --version
      OUTPUT_VARIABLE _stable_rustc_version
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_stable_rustc_version MATCHES "^rustc ${_rust_pinned_version} ")
      list(APPEND _rust_build_env "RUSTUP_TOOLCHAIN=stable")
      message(STATUS "[rust] reusing installed stable alias for pinned rustc ${_rust_pinned_version}")
    endif()
  endif()
endif()
if(APPLE)
  if(OB_MACOS27)
    list(APPEND _rust_build_env "DEVELOPER_DIR=${OB_MACOS_DEVELOPER_DIR}")
  endif()
  # CMake injects -isysroot into its own compile rules on Apple; the cc crate
  # gets no such implicit flag, so the vendored devtools clang cannot find the
  # macOS SDK headers (TargetConditionals.h). SDKROOT is the env var the clang
  # driver itself honors.
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND _rust_build_env
      "SDKROOT=${SEEKDB_IOS_SDK_PATH}"
      "IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  elseif(CMAKE_OSX_SYSROOT)
    list(APPEND _rust_build_env "SDKROOT=${CMAKE_OSX_SYSROOT}")
  else()
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE _macos_sdk_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_macos_sdk_path)
      list(APPEND _rust_build_env "SDKROOT=${_macos_sdk_path}")
    endif()
  endif()
endif()

set(_rust_job_server_options)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
  # Preserve GNU Make's jobserver file descriptors for Cargo. Without this,
  # Cargo sees --jobserver-auth in MAKEFLAGS but cannot use the closed FDs.
  list(APPEND _rust_job_server_options JOB_SERVER_AWARE TRUE)
endif()

add_custom_command(
  OUTPUT "${RUST_STATICLIB}"
  BYPRODUCTS
             "${RUST_TARGET_DIR}/include/config_bridge.h"
             "${RUST_TARGET_DIR}/include/config_checkers.h"
  COMMAND "${CMAKE_COMMAND}" -E env ${_rust_build_env}
          "${CARGO}" build ${_cargo_profile_flag} ${_cargo_target_args}
          --manifest-path "${RUST_WORKSPACE_DIR}/Cargo.toml"
          --package seekdb-host ${_rust_host_features}
  COMMAND "${CMAKE_COMMAND}" -E touch "${RUST_STATICLIB}"
  WORKING_DIRECTORY "${RUST_WORKSPACE_DIR}"
  DEPENDS ${_rust_inputs}
  COMMENT "[rust] cargo build seekdb-host (${_cargo_out_subdir})"
  ${_rust_job_server_options}
  VERBATIM)

add_custom_target(seekdb_rust_host_build DEPENDS "${RUST_STATICLIB}")
add_custom_target(sql_nio_build DEPENDS seekdb_rust_host_build)

add_custom_target(rust_staticlibs_build DEPENDS seekdb_rust_host_build)

# System libraries the Rust std staticlib depends on.
if(WIN32)
  # Win32 libs Rust std's staticlib needs. windows-sys uses raw #[link] that does
  # not emit a /DEFAULTLIB directive for all of these, so list them explicitly --
  # notably ntdll for std's anonymous-pipe path (NtCreateNamedPipeFile).
  set(_rust_syslibs ntdll userenv ws2_32 bcrypt advapi32 synchronization)
else()
  find_package(Threads REQUIRED)
  set(_rust_syslibs Threads::Threads ${CMAKE_DL_LIBS} m)
  if(NOT APPLE AND NOT ANDROID)
    list(APPEND _rust_syslibs rt)
  endif()
endif()

add_library(seekdb_rust_host INTERFACE)
add_dependencies(seekdb_rust_host seekdb_rust_host_build)
target_include_directories(seekdb_rust_host INTERFACE
  "${RUST_WORKSPACE_DIR}/plugin-runtime/include")
target_link_libraries(seekdb_rust_host INTERFACE "${RUST_STATICLIB}" ${_rust_syslibs})

add_library(sql_nio INTERFACE)
target_include_directories(sql_nio INTERFACE "${SQL_NIO_INCLUDE_DIR}")
target_link_libraries(sql_nio INTERFACE seekdb_rust_host)

add_library(config INTERFACE)
target_include_directories(config INTERFACE
  "${CONFIG_INCLUDE_DIR}" "${RUST_TARGET_DIR}/include")
target_link_libraries(config INTERFACE seekdb_rust_host)

set_property(DIRECTORY APPEND PROPERTY
  ADDITIONAL_CLEAN_FILES "${RUST_TARGET_DIR}")

message(STATUS "[rust] seekdb Rust host ready -> ${RUST_STATICLIB}")
