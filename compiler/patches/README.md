# LLVM DirectX graphics output fixes

The pinned LLVM revision has three gaps that prevent its DirectX backend from
producing a valid graphics shader container:

- `DXILOpBuilder.cpp` stores stage flags in a 16-bit local variable, dropping
  the vertex flag at bit 16.
- `DXILTranslateMetadata.cpp` does not copy semantic signatures into DXIL
  entry point metadata.
- `DXContainerGlobals.cpp` writes empty ISG1, OSG1, and PSV0 signature data.

`llvm-directx-graphics.patch` fixes these for semantic signatures described by
LLVM's `dx.semantic.signatures` metadata. The compiler then uses the DXIL
validator to validate and sign the resulting container. The patch does not
change the pinned submodule commit.

Apply it from the `kelyra` repository root before building the DirectX target:

```powershell
git -C third_party/llvm-project apply ../../patches/llvm-directx-graphics.patch
```

The patch is already applied if this check succeeds:

```powershell
git -C third_party/llvm-project apply --reverse --check ../../patches/llvm-directx-graphics.patch
```
