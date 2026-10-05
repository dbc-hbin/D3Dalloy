// This file is intentionally compiled with the host Windows MSVC target ABI.
// It contains only the NVIDIA parameter vtable ABI and forwards all storage to
// C callbacks implemented by the normal MinGW frontend translation unit.

struct ID3D11Resource;
struct ID3D12Resource;

using NgxResult = unsigned int;

extern "C" {
void owned_ngxd3d12_param_set_ull(void *, const char *, unsigned long long);
void owned_ngxd3d12_param_set_float(void *, const char *, float);
void owned_ngxd3d12_param_set_double(void *, const char *, double);
void owned_ngxd3d12_param_set_uint(void *, const char *, unsigned int);
void owned_ngxd3d12_param_set_int(void *, const char *, int);
void owned_ngxd3d12_param_set_d3d11(void *, const char *, ID3D11Resource *);
void owned_ngxd3d12_param_set_d3d12(void *, const char *, ID3D12Resource *);
void owned_ngxd3d12_param_set_pointer(void *, const char *, void *);

NgxResult owned_ngxd3d12_param_get_ull(void *, const char *, unsigned long long *);
NgxResult owned_ngxd3d12_param_get_float(void *, const char *, float *);
NgxResult owned_ngxd3d12_param_get_double(void *, const char *, double *);
NgxResult owned_ngxd3d12_param_get_uint(void *, const char *, unsigned int *);
NgxResult owned_ngxd3d12_param_get_int(void *, const char *, int *);
NgxResult owned_ngxd3d12_param_get_d3d11(void *, const char *, ID3D11Resource **);
NgxResult owned_ngxd3d12_param_get_d3d12(void *, const char *, ID3D12Resource **);
NgxResult owned_ngxd3d12_param_get_pointer(void *, const char *, void **);
void owned_ngxd3d12_param_reset(void *);
}

// Exact virtual order from the pinned public NVSDK_NGX_Parameter declaration.
struct OwnedNgxParameterABI {
  virtual void Set(const char *, unsigned long long) = 0;
  virtual void Set(const char *, float) = 0;
  virtual void Set(const char *, double) = 0;
  virtual void Set(const char *, unsigned int) = 0;
  virtual void Set(const char *, int) = 0;
  virtual void Set(const char *, ID3D11Resource *) = 0;
  virtual void Set(const char *, ID3D12Resource *) = 0;
  virtual void Set(const char *, void *) = 0;

  virtual NgxResult Get(const char *, unsigned long long *) const = 0;
  virtual NgxResult Get(const char *, float *) const = 0;
  virtual NgxResult Get(const char *, double *) const = 0;
  virtual NgxResult Get(const char *, unsigned int *) const = 0;
  virtual NgxResult Get(const char *, int *) const = 0;
  virtual NgxResult Get(const char *, ID3D11Resource **) const = 0;
  virtual NgxResult Get(const char *, ID3D12Resource **) const = 0;
  virtual NgxResult Get(const char *, void **) const = 0;

  virtual void Reset() = 0;
};

class OwnedNgxParameterFacade final : public OwnedNgxParameterABI {
public:
  explicit OwnedNgxParameterFacade(void *storage) : storage_(storage) {}

  void Set(const char *name, unsigned long long value) final { owned_ngxd3d12_param_set_ull(storage_, name, value); }
  void Set(const char *name, float value) final { owned_ngxd3d12_param_set_float(storage_, name, value); }
  void Set(const char *name, double value) final { owned_ngxd3d12_param_set_double(storage_, name, value); }
  void Set(const char *name, unsigned int value) final { owned_ngxd3d12_param_set_uint(storage_, name, value); }
  void Set(const char *name, int value) final { owned_ngxd3d12_param_set_int(storage_, name, value); }
  void Set(const char *name, ID3D11Resource *value) final { owned_ngxd3d12_param_set_d3d11(storage_, name, value); }
  void Set(const char *name, ID3D12Resource *value) final { owned_ngxd3d12_param_set_d3d12(storage_, name, value); }
  void Set(const char *name, void *value) final { owned_ngxd3d12_param_set_pointer(storage_, name, value); }

  NgxResult Get(const char *name, unsigned long long *value) const final { return owned_ngxd3d12_param_get_ull(storage_, name, value); }
  NgxResult Get(const char *name, float *value) const final { return owned_ngxd3d12_param_get_float(storage_, name, value); }
  NgxResult Get(const char *name, double *value) const final { return owned_ngxd3d12_param_get_double(storage_, name, value); }
  NgxResult Get(const char *name, unsigned int *value) const final { return owned_ngxd3d12_param_get_uint(storage_, name, value); }
  NgxResult Get(const char *name, int *value) const final { return owned_ngxd3d12_param_get_int(storage_, name, value); }
  NgxResult Get(const char *name, ID3D11Resource **value) const final { return owned_ngxd3d12_param_get_d3d11(storage_, name, value); }
  NgxResult Get(const char *name, ID3D12Resource **value) const final { return owned_ngxd3d12_param_get_d3d12(storage_, name, value); }
  NgxResult Get(const char *name, void **value) const final { return owned_ngxd3d12_param_get_pointer(storage_, name, value); }

  void Reset() final { owned_ngxd3d12_param_reset(storage_); }

private:
  void *storage_;
};

inline void *operator new(__SIZE_TYPE__, void *memory) noexcept { return memory; }
inline void operator delete(void *, void *) noexcept {}

extern "C" {
int _fltused = 0;

unsigned long long owned_ngxd3d12_msvc_parameter_size() {
  return sizeof(OwnedNgxParameterFacade);
}

unsigned long long owned_ngxd3d12_msvc_parameter_align() {
  return alignof(OwnedNgxParameterFacade);
}

void *owned_ngxd3d12_msvc_parameter_construct(void *memory, void *storage) {
  if (!memory || !storage)
    return nullptr;
  return new (memory) OwnedNgxParameterFacade(storage);
}

void owned_ngxd3d12_msvc_parameter_destruct(void *object) {
  if (object)
    static_cast<OwnedNgxParameterFacade *>(object)->~OwnedNgxParameterFacade();
}
}
