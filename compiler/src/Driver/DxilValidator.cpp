#include "Driver/DxilValidator.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace kelyra::driver {

#ifdef _WIN32
namespace {

// This small ABI slice follows Microsoft's DirectXShaderCompiler dxcapi.h:
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/dxcapi.h
// DXC is distributed under the University of Illinois/NCSA Open Source License.
// These declarations avoid a compile-time dependency on an external DXC SDK;
// the validator is loaded only when producing DXIL on Windows.
struct DxcBlob : IUnknown {
  virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
  virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

struct DxcBlobEncoding : DxcBlob {
  virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL *, UINT32 *) = 0;
};

struct DxcUtils : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateBlobFromBlob(DxcBlob *, UINT32,
                                                       UINT32, DxcBlob **) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  CreateBlobFromPinned(LPCVOID, UINT32, UINT32, DxcBlobEncoding **) = 0;
};

struct DxcOperationResult : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT *) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResult(DxcBlob **) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(DxcBlobEncoding **) = 0;
};

struct DxcValidator : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Validate(DxcBlob *, UINT32,
                                             DxcOperationResult **) = 0;
};

constexpr GUID ClsidDxcUtils = {
    0x6245d6af,
    0x66e0,
    0x48fd,
    {0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c}};
constexpr GUID IidDxcUtils = {0x4605c4cb,
                              0x2019,
                              0x492a,
                              {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};
constexpr GUID ClsidDxcValidator = {
    0x8ca3e215,
    0xf728,
    0x4cf3,
    {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
constexpr GUID IidDxcValidator = {
    0xa6e82bd2,
    0x1fd7,
    0x4826,
    {0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a}};

using DxcCreateInstance = HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *);

struct LoadedLibrary {
  HMODULE Handle = nullptr;
  ~LoadedLibrary() {
    if (Handle)
      FreeLibrary(Handle);
  }
};

std::string Hex(HRESULT Status) {
  char Buffer[16];
  std::snprintf(Buffer, sizeof(Buffer), "0x%08X", unsigned(Status));
  return Buffer;
}

} // namespace
#endif

bool ValidateAndSignDxil(llvm::StringRef Path, std::string &Error) {
#ifndef _WIN32
  Error = "DXIL validation and signing requires Windows";
  return false;
#else
  std::ifstream Input(Path.str(), std::ios::binary);
  if (!Input) {
    Error = "cannot read LLVM DXContainer for validation";
    return false;
  }
  std::vector<char> Bytes((std::istreambuf_iterator<char>(Input)),
                          std::istreambuf_iterator<char>());
  if (Bytes.empty() || Bytes.size() > std::numeric_limits<UINT32>::max()) {
    Error = "LLVM DXContainer is empty or too large for DXIL validator";
    return false;
  }
  LoadedLibrary Library;
  Library.Handle = LoadLibraryW(L"dxcompiler.dll");
  if (!Library.Handle)
    Library.Handle = LoadLibraryW(L"dxil.dll");
  if (!Library.Handle) {
    Error = "DXIL validator is unavailable; install dxcompiler.dll or dxil.dll "
            "and add its directory to PATH";
    return false;
  }
  auto Create = reinterpret_cast<DxcCreateInstance>(
      GetProcAddress(Library.Handle, "DxcCreateInstance"));
  if (!Create) {
    Error = "DXIL validator library has no DxcCreateInstance export";
    return false;
  }
  Microsoft::WRL::ComPtr<DxcUtils> Utils;
  HRESULT Status = Create(ClsidDxcUtils, IidDxcUtils,
                          reinterpret_cast<void **>(Utils.GetAddressOf()));
  if (FAILED(Status)) {
    Error = "cannot create DXIL blob utility: " + Hex(Status);
    return false;
  }
  Microsoft::WRL::ComPtr<DxcValidator> Validator;
  Status = Create(ClsidDxcValidator, IidDxcValidator,
                  reinterpret_cast<void **>(Validator.GetAddressOf()));
  if (FAILED(Status)) {
    Error = "cannot create DXIL validator: " + Hex(Status);
    return false;
  }
  Microsoft::WRL::ComPtr<DxcBlobEncoding> InputBlob;
  Status = Utils->CreateBlobFromPinned(Bytes.data(), UINT32(Bytes.size()), 0,
                                       InputBlob.GetAddressOf());
  if (FAILED(Status)) {
    Error = "cannot pass DXContainer to validator: " + Hex(Status);
    return false;
  }
  Microsoft::WRL::ComPtr<DxcOperationResult> Result;
  Status = Validator->Validate(InputBlob.Get(), 1, Result.GetAddressOf());
  if (FAILED(Status)) {
    Error = "DXIL validator call failed: " + Hex(Status);
    return false;
  }
  HRESULT ValidationStatus = S_OK;
  Status = Result->GetStatus(&ValidationStatus);
  if (FAILED(Status) || FAILED(ValidationStatus)) {
    Microsoft::WRL::ComPtr<DxcBlobEncoding> Diagnostics;
    Result->GetErrorBuffer(Diagnostics.GetAddressOf());
    Error = "DXIL validation failed: " +
            Hex(FAILED(Status) ? Status : ValidationStatus);
    if (Diagnostics && Diagnostics->GetBufferPointer())
      Error += "\n" + std::string(static_cast<const char *>(
                                      Diagnostics->GetBufferPointer()),
                                  Diagnostics->GetBufferSize());
    return false;
  }
  Microsoft::WRL::ComPtr<DxcBlob> Signed;
  Status = Result->GetResult(Signed.GetAddressOf());
  if (FAILED(Status) || !Signed || !Signed->GetBufferPointer()) {
    Error = "DXIL validator did not return a signed container: " + Hex(Status);
    return false;
  }
  std::ofstream Output(Path.str(), std::ios::binary | std::ios::trunc);
  if (!Output) {
    Error = "cannot write validated DXContainer";
    return false;
  }
  Output.write(static_cast<const char *>(Signed->GetBufferPointer()),
               Signed->GetBufferSize());
  if (!Output) {
    Error = "cannot finish writing validated DXContainer";
    return false;
  }
  return true;
#endif
}

} // namespace kelyra::driver
