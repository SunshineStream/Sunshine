/**
 * @file src/nvenc/win/impl/nvenc_shared_dll.h
 * @brief Declarations for Windows HMODULE RAII helpers.
 */
#pragma once

#include <memory>
#include <type_traits>

#include <windows.h>

namespace nvenc {

  using shared_dll = std::shared_ptr<std::remove_pointer_t<HMODULE>>;

  struct shared_dll_deleter {
    void
    operator()(HMODULE dll) {
      if (dll) FreeLibrary(dll);
    }
  };

  inline shared_dll
  make_shared_dll(HMODULE dll) {
    return shared_dll(dll, shared_dll_deleter());
  }

}  // namespace nvenc
