/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#pragma once

/*
 * D3D12 shared handles.
 *
 * Every shareable object owns one Wine section named
 * `Local\dxmt.share.<pid>.<128-bit random>`: a 64 KiB header followed by the
 * payload (heap or committed-buffer memory; empty for textures and fences).
 * The NT handle given to the application is a real section handle, so
 * CloseHandle/DuplicateHandle/inheritance are native. The Wine server backs
 * the section with a 0600 file in its per-user 0700 directory and unlinks it
 * right after creation, before any view exists.
 *
 * The header carries a random 128-bit token, the creation descriptors and,
 * for IOSurface textures and MTLSharedEvents, per-process rendezvous slots:
 * each process holding the object registers a private receive right under a
 * fresh `dxmt.share.<pid>.<random>` bootstrap name that answers with the Metal
 * object port. The registration dies with its receive right (last close in
 * that process, or process exit). After import the Metal object's global
 * identity (IOSurfaceID / shared-event trace id) must equal the header's, so a
 * reused or stale name never yields a different object.
 *
 * A D3D object name maps to a link section `Local\dxmt.d3d12.<hash>` where
 * hash = SHA-256("dxmt.d3d12.name", per-user salt, UTF-16 name) truncated to
 * 128 bits. The link holds the token and the main section name; the token is
 * verified again after resolving.
 */

#include "d3d12_shared_metadata.hpp"
#include <cstdint>
#include <memory>
#include <mutex>

namespace dxmt::sharing {

/* One mapped share per section per process; every device of the process reuses it. */
struct SharedObjectAccess;
class SharedObject {
public:
  ~SharedObject();

  Kind kind() const { return header_->kind; }
  const Header &header() const { return *header_; }
  Header &header() { return *header_; }
  uint8_t *payload() const { return payload_; }
  uint64_t payloadSize() const { return header_->payload_size; }

  /* Registers this process' rendezvous for a Metal object port. */
  HRESULT publishPort(uint32_t mach_port);
  /* Resolves an owned object send right from any live holder; 0 on failure. */
  uint32_t lookUpPort();

  HRESULT createHandle(const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, LUID creator_luid,
                       HANDLE *handle);

  SharedObject(const SharedObject &) = delete;
  SharedObject &operator=(const SharedObject &) = delete;

private:
  SharedObject() = default;
  friend struct SharedObjectAccess;

  void lockSlots();
  void unlockSlots();

  HANDLE section_ = nullptr;
  WCHAR section_name_[80] = {};
  Header *header_ = nullptr;
  uint8_t *payload_ = nullptr;
  uint64_t rendezvous_ = 0;
  int32_t slot_ = -1;
  std::mutex mutex_;
};

/* Creates a section with a zero-filled payload of `payload_size` bytes (64 KiB granular). */
HRESULT Create(Kind kind, uint64_t payload_size, std::shared_ptr<SharedObject> &out);
/* Validates a handle (main or named link section) and returns the process-wide share. */
HRESULT Open(HANDLE handle, std::shared_ptr<SharedObject> &out);
HRESULT OpenByName(const WCHAR *name, DWORD access, HANDLE *handle);

} // namespace dxmt::sharing
