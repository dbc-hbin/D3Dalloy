// Compile-only ABI oracle: unresolved methods are intentional. The compiler
// emits this vftable from NVIDIA's base class, independently of the facade.
#include <nvsdk_ngx_params.h>

class OwnedNgxParameterFacade final : public NVSDK_NGX_Parameter {
public:
  void Set(const char *, unsigned long long) final;
  void Set(const char *, float) final;
  void Set(const char *, double) final;
  void Set(const char *, unsigned int) final;
  void Set(const char *, int) final;
  void Set(const char *, ID3D11Resource *) final;
  void Set(const char *, ID3D12Resource *) final;
  void Set(const char *, void *) final;
  NVSDK_NGX_Result Get(const char *, unsigned long long *) const final;
  NVSDK_NGX_Result Get(const char *, float *) const final;
  NVSDK_NGX_Result Get(const char *, double *) const final;
  NVSDK_NGX_Result Get(const char *, unsigned int *) const final;
  NVSDK_NGX_Result Get(const char *, int *) const final;
  NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const final;
  NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const final;
  NVSDK_NGX_Result Get(const char *, void **) const final;
  void Reset() final;
};
OwnedNgxParameterFacade ngx_vtable_reference;
