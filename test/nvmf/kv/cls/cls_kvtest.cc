// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab
//
// KVX-3 THROWAWAY test object class for exercising the KV Exec -> rados_aio_exec
// path end to end against a vstart cluster. NOT a production class (ADR-0005
// keeps real cls authoring to a later effort).
//
// Methods:
//   echo     - return the caller's input blob unchanged (no object access).
//   upcase   - read the object's stored value and return it uppercased; if the
//              object is empty/absent, uppercase the caller's input instead.
//   fixedout - return EXACTLY N bytes of output, where N is a little-endian
//              uint32 in the first 4 input bytes (the rest of the input is
//              ignored). Lets the KVX exec e2e drive deterministic EXACT-FIT
//              (N == host buffer) and OVER-LARGE (N > host buffer) outputs to
//              prove the librados backend's true-length / BUFFER_TOO_SMALL
//              semantics. Output byte i is 'A' + (i % 26), no object access.
//
// Build against the Ceph tree at /mnt/ceph; load libcls_kvtest.so into the
// vstart OSD (see test/nvmf/kv/cls/build_and_load.sh).

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <string>

#include "objclass/objclass.h"

CLS_VER(1, 0)
CLS_NAME(kvtest)

// echo: return the input bytes straight back to the caller. Does not touch the
// object, so it is registered RD-only (actually neither RD nor WR is needed,
// but RD is harmless and matches the "may read" contract).
static int kvtest_echo(cls_method_context_t hctx, ceph::bufferlist *in,
                       ceph::bufferlist *out)
{
  CLS_LOG(10, "kvtest echo: %u input bytes", (unsigned)in->length());
  *out = *in;
  return 0;
}

// upcase: read the stored object value and return it uppercased. If the object
// has no value, fall back to uppercasing the input blob. Reads the object, so
// it is registered RD.
static int kvtest_upcase(cls_method_context_t hctx, ceph::bufferlist *in,
                         ceph::bufferlist *out)
{
  ceph::bufferlist stored;
  int r = cls_cxx_read(hctx, 0, 0, &stored);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("kvtest upcase: read failed: %d", r);
    return r;
  }

  std::string s;
  if (stored.length() > 0) {
    s.assign(stored.c_str(), stored.length());
  } else {
    s.assign(in->c_str(), in->length());
  }
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });

  out->clear();
  out->append(s.data(), s.size());
  CLS_LOG(10, "kvtest upcase: returning %u bytes", (unsigned)out->length());
  return 0;
}

// Hard cap on a fixedout request (8 MiB). A host could otherwise pass a huge N
// (up to ~4 GiB) and drive the OSD to allocate that much, exhausting OSD memory.
// 8 MiB comfortably exceeds the backend's 1 MiB exec-output cap so the e2e can
// still exercise the >cap / OVER-LARGE path while keeping the OSD safe.
#define KVTEST_FIXEDOUT_MAX (8u * 1024u * 1024u)

// fixedout: emit EXACTLY N bytes of deterministic output where N is the LE
// uint32 in the first 4 input bytes. Does not touch the object. Used to drive
// EXACT-FIT and OVER-LARGE KV Exec outputs from the e2e host. N over
// KVTEST_FIXEDOUT_MAX is rejected with -EINVAL so a host cannot exhaust OSD RAM.
static int kvtest_fixedout(cls_method_context_t hctx, ceph::bufferlist *in,
                           ceph::bufferlist *out)
{
  uint32_t n = 0;
  if (in->length() >= 4) {
    const unsigned char *p = (const unsigned char *)in->c_str();
    n = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
  if (n > KVTEST_FIXEDOUT_MAX) {
    CLS_ERR("kvtest fixedout: requested %u bytes exceeds cap %u", n,
            KVTEST_FIXEDOUT_MAX);
    return -EINVAL;
  }
  out->clear();
  std::string s;
  s.resize(n);
  for (uint32_t i = 0; i < n; i++) {
    s[i] = (char)('A' + (i % 26));
  }
  out->append(s.data(), s.size());
  CLS_LOG(10, "kvtest fixedout: returning %u bytes", (unsigned)out->length());
  return 0;
}

CLS_INIT(kvtest)
{
  CLS_LOG(0, "loading cls_kvtest");

  cls_handle_t h_class;
  cls_method_handle_t h_echo;
  cls_method_handle_t h_upcase;
  cls_method_handle_t h_fixedout;

  cls_register("kvtest", &h_class);
  detail::cls_register_cxx_method_impl(h_class, "echo", CLS_METHOD_RD,
                                       kvtest_echo, &h_echo);
  detail::cls_register_cxx_method_impl(h_class, "upcase", CLS_METHOD_RD,
                                       kvtest_upcase, &h_upcase);
  detail::cls_register_cxx_method_impl(h_class, "fixedout", CLS_METHOD_RD,
                                       kvtest_fixedout, &h_fixedout);
}
